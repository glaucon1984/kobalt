#include "firmware_power.h"
#include <QtGlobal>
#include <QCoreApplication>
#include <QFile>
#include <QString>
#include <dlfcn.h>
#include <inkview.h>
#include <time.h>

namespace kobalt::pocketbook {
namespace {
// Field offsets in firmware U710.6.11's libinkview.so, read off SetAutoPowerOff and
// iv_rise_poweroff_timer: both take ivstate.powerofftm at +0x68 (milliseconds: the timer is
// armed with powerofftm - (hw_boottime() - last activity) * 1000; seen as 600000 for the
// 10-minute setting), store the enable byte at +0x90, and read the last activity, in
// hw_boottime seconds, at *(ivmpc)+0x16c. The SDK's inkinternal.h (6.5) lays iv_state out the
// same way up to there (keylocktm, powerofftm, ..., poweroffenable), but the device's library
// is what counts, hence the lookup by symbol with these offsets.
const int powerOffMillisOffset = 0x68;
const int powerOffEnabledOffset = 0x90;
const int lastActivityOffset = 0x16c;

struct Symbols {
    const char *state = nullptr;
    const char *const *mpctl = nullptr;
    long (*bootTime)() = nullptr;
    bool looked = false;
};
Symbols &symbols() {
    static Symbols s;
    if (!s.looked) {
        s.looked = true;
        s.state = reinterpret_cast<const char *>(dlsym(RTLD_DEFAULT, "ivstate"));
        s.mpctl = reinterpret_cast<const char *const *>(dlsym(RTLD_DEFAULT, "ivmpc"));
        s.bootTime = reinterpret_cast<long (*)()>(dlsym(RTLD_DEFAULT, "hw_boottime"));
        if (!s.state || !s.mpctl || !s.bootTime)
            qWarning("kobalt-pb: timed power-off state not found in libinkview (ivstate %p, ivmpc %p, hw_boottime %p)",
                     static_cast<const void *>(s.state), static_cast<const void *>(s.mpctl),
                     reinterpret_cast<void *>(s.bootTime));
    }
    return s;
}
}

int secondsToTimedPowerOff() {
    const Symbols &s = symbols();
    if (!s.state || !s.mpctl || !*s.mpctl || !s.bootTime)
        return INT_MIN;
    const int timeout = *reinterpret_cast<const int *>(s.state + powerOffMillisOffset);
    const unsigned char enabled = *reinterpret_cast<const unsigned char *>(s.state + powerOffEnabledOffset);
    const long lastActivity = *reinterpret_cast<const long *>(*s.mpctl + lastActivityOffset);
    const long now = s.bootTime();
    // Plausibility: a timeout between a minute and a week, a flag that is a flag, an activity in
    // the past within the last 30 days of uptime. Anything else means the layout is not the one
    // these offsets were taken from, and the caller treats every wake as a press.
    if (enabled > 1 || timeout < 60000 || timeout > 7 * 86400000 || lastActivity < 0 || lastActivity > now ||
        now - lastActivity > 30L * 86400) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            qWarning("kobalt-pb: timed power-off state implausible (timeout %d, enabled %d, last activity %ld, now %ld)",
                     timeout, enabled, lastActivity, now);
        }
        return INT_MIN;
    }
    if (!enabled)
        return INT_MIN;
    const long remaining = timeout / 1000 - (now - lastActivity);
    return int(qBound(long(INT_MIN + 1), remaining, long(INT_MAX)));
}

bool frontlightEnabled() {
    return GetFrontlightEnabled() > 0;
}

int frontlightLevel() {
    return qMax(0, GetFrontlightState());
}

void setFrontlightEnabled(bool enabled) {
    SetFrontlightEnabled(enabled ? 1 : 0);
}

void setFrontlightLevel(int level) {
    SetFrontlightState(qBound(0, level, 100));
}

void applyFrontlight(int level) {
    // As KOReader's PocketBook powerd does: the flag first, so the level is applied to a light
    // that is on; off is the level zero plus the flag, so the firmware's own panel shows it off.
    if (level > 0) {
        setFrontlightEnabled(true);
        setFrontlightLevel(level);
    } else {
        setFrontlightLevel(0);
        setFrontlightEnabled(false);
    }
}

namespace {
struct AutoBrightness {
    int (*supported)() = nullptr;
    int (*enabled)() = nullptr;
    void (*setEnabled)(int) = nullptr;
    bool looked = false;
};
AutoBrightness &autoBrightness() {
    static AutoBrightness a;
    if (!a.looked) {
        a.looked = true;
        a.supported = reinterpret_cast<int (*)()>(dlsym(RTLD_DEFAULT, "hw_is_frontlight_auto_supported"));
        a.enabled = reinterpret_cast<int (*)()>(dlsym(RTLD_DEFAULT, "hw_is_frontlight_auto"));
        a.setEnabled = reinterpret_cast<void (*)(int)>(dlsym(RTLD_DEFAULT, "hw_set_frontlight_auto_enabled"));
    }
    return a;
}
}

bool hasAutomaticBrightness() {
    const AutoBrightness &a = autoBrightness();
    // 0 none, 1 a light sensor, 2 the schedule by time (support_fl_time in device.cfg).
    return a.supported && a.enabled && a.setEnabled && a.supported() != 0;
}

bool automaticBrightness() {
    return hasAutomaticBrightness() && autoBrightness().enabled() != 0;
}

bool setAutomaticBrightness(bool enabled) {
    if (!hasAutomaticBrightness())
        return false;
    // The schedule drives the level of a light that is on; switched off (the slider at zero
    // clears the firmware's flag), the schedule alone leaves it off.
    if (enabled && !frontlightEnabled())
        setFrontlightEnabled(true);
    autoBrightness().setEnabled(enabled ? 1 : 0);
    return automaticBrightness() == enabled;
}

namespace {
QString standbyLightPath() {
    return QCoreApplication::applicationDirPath() + QStringLiteral("/light.standby");
}
}

int darkenForStandby() {
    if (!frontlightEnabled())
        return -1;
    const int level = frontlightLevel();
    const bool scheduled = automaticBrightness();
    QFile record(standbyLightPath());
    if (record.open(QIODevice::WriteOnly)) {
        record.write(QByteArray::number(level) + (scheduled ? " auto" : ""));
        record.close();
    }
    if (scheduled)
        setAutomaticBrightness(false);
    applyFrontlight(0);
    return level;
}

bool restoreStandbyLight(const char *why) {
    QFile record(standbyLightPath());
    if (!record.open(QIODevice::ReadOnly))
        return false;
    const QList<QByteArray> fields = record.readAll().trimmed().split(' ');
    record.close();
    QFile::remove(standbyLightPath());
    const int level = fields.isEmpty() ? 0 : fields[0].toInt();
    const bool scheduled = fields.size() > 1 && fields[1] == "auto";
    if (scheduled) {
        // Whatever the level was (the curve may have been at zero at the standby): the light on
        // and the schedule with it, and the firmware applies its curve's value for now.
        setAutomaticBrightness(true);
        qInfo("kobalt-pb: frontlight back on with the schedule (%s)", why);
        return true;
    }
    if (level <= 0)
        return false;
    applyFrontlight(level);
    qInfo("kobalt-pb: frontlight back on at %d (%s)", level, why);
    return true;
}
}

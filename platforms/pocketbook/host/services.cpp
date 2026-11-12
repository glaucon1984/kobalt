#include "services.h"
#include "host.h"
#include "sleep_screen_files.h"
#include "app/reader/views/annotation_exporter.h"
#include "ui/controls/keyboard.h"
#include "ui/style/fonts.h"
#include <QGuiApplication>
#include <qpa/qplatformnativeinterface.h>
#include <QScreen>
#include <QFile>
#include <QKeyEvent>
#include <QPointer>
#include <QTime>
#include <QTimer>
#include <QElapsedTimer>
#include "firmware_power.h"
#include <dlfcn.h>
#include <inkview.h>
#include <memory>

namespace kobalt::pocketbook {
namespace {
// inkview: GetFrontlightState() is the brightness 0..100 (negative when the light is off or
// absent), GetFrontlightColor()/SetFrontlightColor() the warmth 0..100 on devices with one.
// The edge drag calls setGestureLevel/setWarmth for every finger movement; the firmware's
// calls are not instant (they are timed below and logged when slow), so the hardware is
// written at most every lightWriteIntervalMs with the latest value, and the notice on screen
// follows the finger instead of a queue of light changes.
// The warmth schedule (system/config/frontlight/autocolor.cfg) is run by the firmware, not by
// Kobalt: libinkview's hw_*_frontlight_color_auto calls read and set its flag in the shared
// state and tell the firmware. They are not in inkview.h, so they are looked up at run time
// and a firmware without them loses only the option. SetFrontlightColor leaves the flag alone,
// so a manual change clears it, or the firmware would overwrite the value.
const int lightWriteIntervalMs = 120;
template <typename Call>
void timedLight(const char *what, int value, Call call) {
    QElapsedTimer clock;
    clock.start();
    call();
    if (clock.elapsed() > 20)
        qInfo("kobalt-pb: %s(%d) took %lld ms", what, value, static_cast<long long>(clock.elapsed()));
}
class Light final : public LightControls {
public:
    Light() {
        flush.setSingleShot(true);
        QObject::connect(&flush, &QTimer::timeout, [this] { writePending(); });
        scheduleResume.setSingleShot(true);
        scheduleResume.setInterval(scheduleResumeMs);
        QObject::connect(&scheduleResume, &QTimer::timeout, [this] { resumeSchedule(); });
        autoColor = reinterpret_cast<int (*)()>(dlsym(RTLD_DEFAULT, "hw_is_frontlight_color_auto"));
        setAutoColor = reinterpret_cast<void (*)(int)>(dlsym(RTLD_DEFAULT, "hw_set_frontlight_color_auto_enabled"));
    }
    bool initialize() override {
        warm = GetFrontlightColor() >= 0;
        original = brightness();
        return true;
    }
    bool hasWarmth() const override {
        return warm;
    }
    int warmth() const override {
        return qMax(0, GetFrontlightColor());
    }
    int warmthMinimum() const override {
        return 0;
    }
    int warmthMaximum() const override {
        return 100;
    }
    int warmthStep() const override {
        return 1;
    }
    bool hasAutomaticBrightness() const override {
        return pocketbook::hasAutomaticBrightness();
    }
    bool automaticBrightness() const override {
        return pocketbook::automaticBrightness();
    }
    bool setAutomaticBrightness(bool enabled) override {
        if (!hasAutomaticBrightness())
            return false;
        if (enabled)
            pendingBrightness = -1; // from now on the level is the firmware's
        bool applied = false;
        timedLight("hw_set_frontlight_auto_enabled", enabled,
                   [enabled, &applied] { applied = pocketbook::setAutomaticBrightness(enabled); });
        qInfo("kobalt-pb: brightness schedule %s%s, level %d", enabled ? "on" : "off", applied ? "" : " NOT applied",
              frontlightLevel());
        return applied;
    }
    bool hasAutomaticWarmth() const override {
        return warm && autoColor && setAutoColor;
    }
    bool automaticWarmth() const override {
        return hasAutomaticWarmth() && autoColor() != 0;
    }
    int setWarmth(int value) override {
        value = qBound(0, value, 100);
        if (automaticWarmth())
            setAutomaticWarmth(false);
        // The brightness schedule would hold the colour back until something re-applies the light.
        bool lifted = false;
        timedLight("lift brightness schedule", value, [&lifted] { lifted = pocketbook::liftBrightnessSchedule(); });
        if (lifted)
            qInfo("kobalt-pb: brightness schedule lifted for a warmth change");
        if (lifted || scheduleResume.isActive())
            scheduleResume.start();
        pendingWarmth = value;
        schedule();
        return value;
    }
    bool saveWarmth() override {
        return true;
    }
    bool setAutomaticWarmth(bool enabled) override {
        if (!hasAutomaticWarmth())
            return false;
        if (enabled)
            pendingWarmth = -1; // from now on the warmth is the firmware's
        timedLight("hw_set_frontlight_color_auto_enabled", enabled,
                   [this, enabled] { setAutoColor(enabled ? 1 : 0); });
        const bool applied = automaticWarmth() == enabled;
        qInfo("kobalt-pb: warmth schedule %s%s, warmth %d", enabled ? "on" : "off", applied ? "" : " NOT applied",
              warmth());
        return applied;
    }
    bool beginGesture() override {
        original = brightness();
        active = true;
        return true;
    }
    int setGestureLevel(int percent) override {
        percent = qBound(0, percent, 100);
        // The firmware keeps a level set while its schedule runs without applying it.
        if (automaticBrightness())
            setAutomaticBrightness(false);
        pendingBrightness = percent;
        schedule();
        return percent;
    }
    void finishGesture(bool save) override {
        // Only a cancelled gesture restores; a stray finish (e.g. at close) must not touch the light.
        if (active && !save) {
            pendingBrightness = -1;
            timedLight("applyFrontlight", original, [this] { applyFrontlight(original); });
        }
        active = false;
        writePending(); // the last value of a saved gesture goes out now
    }
    int brightness() const override {
        // The firmware keeps a level beside an on/off flag (its panel, and a power-off while
        // Kobalt had the light off for a standby, switch the flag): off reads as zero here, so
        // that the slider shows the light as it is and dragging it up switches the light on.
        return frontlightEnabled() ? frontlightLevel() : 0;
    }
    bool gesturing() const override {
        return active;
    }

    ~Light() override {
        resumeSchedule();
    }

private:
    static constexpr int scheduleResumeMs = 1000;
    QTimer scheduleResume;
    void resumeSchedule() {
        scheduleResume.stop();
        writePending(); // the colour goes out while the schedule is still lifted
        bool resumed = false;
        timedLight("resume brightness schedule", 1, [&resumed] { resumed = pocketbook::resumeBrightnessSchedule(); });
        if (resumed)
            qInfo("kobalt-pb: brightness schedule resumed after a warmth change, level %d", frontlightLevel());
    }
    bool warm = false, active = false;
    int original = 0;
    int pendingBrightness = -1, pendingWarmth = -1;
    int (*autoColor)() = nullptr;
    void (*setAutoColor)(int) = nullptr;
    QTimer flush;
    QElapsedTimer lastWrite;
    void schedule() {
        if (flush.isActive())
            return;
        const qint64 since = lastWrite.isValid() ? lastWrite.elapsed() : lightWriteIntervalMs;
        if (since >= lightWriteIntervalMs)
            writePending();
        else
            flush.start(int(lightWriteIntervalMs - since));
    }
    void writePending() {
        flush.stop();
        if (pendingBrightness >= 0) {
            const int value = pendingBrightness;
            pendingBrightness = -1;
            timedLight("applyFrontlight", value, [value] { applyFrontlight(value); });
            lastWrite.start();
        }
        if (pendingWarmth >= 0) {
            const int value = pendingWarmth;
            pendingWarmth = -1;
            timedLight("SetFrontlightColor", value, [value] { SetFrontlightColor(value); });
            lastWrite.start();
        }
    }
};
class Dictionary final : public DictionaryService {
public:
    QVector<DictionaryChoice> choices() const override {
        return {};
    }
    bool select(const QString &, const QString &, QString *error) override {
        if (error)
            *error = QStringLiteral("No dictionary installed.");
        return false;
    }
    DictionaryResult lookup(const QString &, const QString &word) override {
        DictionaryResult result;
        result.word = word;
        result.error = QStringLiteral("No dictionary installed.");
        return result;
    }
};
// Sleep screen: the firmware draws the lock screen from a cached bitmap, so Kobalt's sleep
// image is written to that file for the *next* standby (the host runs Kobalt's sleep sequence
// at book open and at every EVT_BACKGROUND) and the task manager reloads it at wake.
class Presentation final : public WindowPresentation {
public:
    void pauseBackground(bool) override {}
    void resumeBackground() override {}
    void resumeAndRepaintBackground() override {}
    bool supportsSleepScreen() const override {
        return true;
    }
    bool presentSleepScreen(QImage image, Qt::ScreenOrientation) override {
        if (!writeLockImages(image))
            return false;
        markLockReloadPending();
        return true;
    }
    bool updateSleepScreen(QImage, const QRect &) override {
        return false;
    }
    void clearSleepScreen() override {}
    void acceptHandoff(QDialog &, const std::function<void()> &accept) override {
        accept();
    }
};
// KOsync's connection: the firmware keeps Wi-Fi off until an app asks for it, and inkview's
// NetConnect (with the firmware's own network dialog when it has to ask) or NetConnectSilent
// blocks until the link is up or given up. The call is made from the event loop after the
// caller returned, as a native Wi-Fi workflow would be; a cancelled wait ignores the result.
class Network final : public SyncNetwork {
public:
    ~Network() override {
        cancel();
    }
    bool connected() const override {
        return (QueryNetwork() & NET_CONNECTED) != 0;
    }
    // Interactive (Sync now, sign-in): the firmware's own connection dialog, blocking as the
    // firmware wants it. Silent (sync at book open and wake): in the background, the reader
    // stays usable while the firmware looks for a known network; a cancelled wait (the book
    // closes, another request) just drops the answer.
    void prepare(bool interactive, std::function<void(bool)> finished) override {
        cancel();
        if (connected()) {
            finished(true);
            return;
        }
        auto token = std::make_shared<bool>(true);
        waiting = token;
        if (!interactive) {
            auto *native = QGuiApplication::platformNativeInterface();
            if (auto fn = native ? native->platformFunction("pbinkConnectNetworkAsync") : nullptr) {
                struct Pending {
                    std::shared_ptr<bool> token;
                    std::function<void(bool)> finished;
                };
                auto *pending = new Pending{token, std::move(finished)};
                reinterpret_cast<void (*)(void (*)(int, void *), void *)>(fn)(
                    [](int connected, void *context) {
                        std::unique_ptr<Pending> pending(static_cast<Pending *>(context));
                        if (*pending->token)
                            pending->finished(connected == 1);
                    },
                    pending);
                return;
            }
        }
        QTimer::singleShot(0, qApp, [token, interactive, finished = std::move(finished)] {
            if (!*token)
                return;
            const bool ready = Host::connectNetwork(interactive);
            if (*token)
                finished(ready);
        });
    }
    void cancel() override {
        if (waiting)
            *waiting = false;
        waiting.reset();
    }

private:
    std::shared_ptr<bool> waiting;
};
// Link quality 1..4 from the kernel (/proc/net/wireless, "link" column, 0..70 on this driver);
// 4 when the file says nothing, so a connected link always shows.
int wirelessStrength() {
    QFile file(QStringLiteral("/proc/net/wireless"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return 4;
    const auto lines = QString::fromLatin1(file.readAll()).split('\n');
    for (const auto &line : lines) {
        const int colon = line.indexOf(':');
        if (colon < 0 || !line.left(colon).trimmed().startsWith(QLatin1String("wlan")))
            continue;
        auto fields = line.mid(colon + 1).split(' ', Qt::SkipEmptyParts);
        if (fields.size() < 2)
            return 4;
        bool ok = false;
        const double link = fields[1].remove('.').toDouble(&ok);
        if (!ok || link <= 0)
            return 4;
        return qBound(1, int((link * 4 + 69) / 70), 4);
    }
    return 4;
}
// The reading menu's Wi-Fi indicator: inkview's network state, polled every few seconds (the
// firmware sends the task no event for it), with the kernel's link quality.
void observeWifi(QObject *context, std::function<void(WifiStatus)> changed) {
    auto last = std::make_shared<WifiStatus>();
    last->strength = -2; // never published yet
    auto poll = [changed, last] {
        WifiStatus status;
        const int state = QueryNetwork();
        status.enabled = (state & NET_WIFIREADY) != 0 || (state & NET_CONNECTED) != 0;
        status.connected = (state & NET_CONNECTED) != 0;
        status.strength = status.connected ? wirelessStrength() : -1;
        if (status.enabled == last->enabled && status.connected == last->connected &&
            status.strength == last->strength)
            return;
        *last = status;
        changed(status);
    };
    auto *timer = new QTimer(context);
    QObject::connect(timer, &QTimer::timeout, context, poll);
    timer->start(5000);
    poll();
}
// "PocketBook 710" and the like from inkview, for KOsync's default device name.
QString deviceName() {
    const char *model = GetDeviceModel();
    QString name = model ? QString::fromUtf8(model).trimmed() : QString();
    if (name.isEmpty())
        return QStringLiteral("PocketBook");
    if (!name.startsWith(QLatin1String("PocketBook"), Qt::CaseInsensitive))
        name = QStringLiteral("PocketBook ") + name;
    return name;
}
}

ReaderHostServices readerServices() {
    ReaderHostServices result;
    result.desktopUi = false;
    result.systemRotation = false;
    result.downloadableDictionaries = true; // Qt is linked with OpenSSL; CA bundle via SSL_CERT_FILE
    result.deviceName = deviceName();
    // Exported annotations (HTML) in a folder of the user's storage; the library lists them.
    result.annotationExporter = [](QWidget *reader) {
        return reader::directoryAnnotationExporter(reader, QStringLiteral("/mnt/ext1/Kobalt/Annotations"));
    };
    // Transparent PNGs for the "PNG over text" sleep screen, next to the profile (the installer
    // leaves the folder alone; the user reaches it over USB).
    result.sleepOverlayDirectories = QStringList{ QStringLiteral("/mnt/ext1/applications/kobalt/screensaver") };
    result.colorDisplay = [] {
        return false;
    };
    result.devicePreview = [] {
        return false;
    };
    result.browserAvailable = [] {
        return false;
    };
    result.openBrowser = [](const QUrl &, QWidget *) {
        return false;
    };
    result.clock = [] {
        return QTime::currentTime().toString("HH:mm");
    };
    result.battery = [] {
        return BatteryStatus{GetBatteryPower(), IsCharging() != 0, false};
    };
    result.observeWifi = observeWifi;
    result.pageButtons = [] {
        return true;
    };
    // The two page buttons, mappable in Settings → Controls → Hardware buttons. The reader matches
    // the pressed key against these, so they carry the keys the pbink plugin delivers, not Kobo's
    // F23/F24. On the Era Lite the upper button is the firmware's "next" key (IV_KEY_NEXT,
    // PageDown here) and the lower one "previous" (IV_KEY_PREV, PageUp). "Upper" and "lower" name
    // the button's position as the device is held, so upside down (or in inverted landscape) the
    // keys cross over: a mapped action then stays with the button on top, as the unmapped default
    // does (its direction is recomputed through pageButtonDirection below, which swaps the same
    // way). The host forwards the keys through physicalPageButton so the mapping applies.
    result.buttonControls = [] {
        bool inverted = false;
        if (const auto *screen = QGuiApplication::primaryScreen())
            inverted = screen->orientation() == Qt::InvertedPortraitOrientation
                    || screen->orientation() == Qt::InvertedLandscapeOrientation;
        const int upperKey = inverted ? Qt::Key_PageUp : Qt::Key_PageDown;
        const int lowerKey = inverted ? Qt::Key_PageDown : Qt::Key_PageUp;
        return pageButtons::Buttons{ { "page_upper", "buttons.upper", upperKey, 1 },
                                     { "page_lower", "buttons.lower", lowerKey, -1 } };
    };
    result.reverseLandscapeButtons = [] {
        return false;
    };
    // The pbink plugin maps the PocketBook's page buttons to PageDown (the firmware's "next", the
    // upper button on the Era Lite) and PageUp ("previous", the lower one). As on Kobo: turned
    // upside down (or inverted landscape), the lower button is the physically upper one, so the
    // pair swaps with the screen orientation.
    result.pageButtonDirection = [](QKeyEvent *event) {
        int direction = 0;
        switch (event->key()) {
        case Qt::Key_PageDown: direction = 1; break;
        case Qt::Key_PageUp: direction = -1; break;
        case Qt::Key_Right: return 1;
        case Qt::Key_Left: return -1;
        default: return 0;
        }
        const auto *screen = QGuiApplication::primaryScreen();
        if (screen && (screen->orientation() == Qt::InvertedPortraitOrientation ||
                       screen->orientation() == Qt::InvertedLandscapeOrientation))
            direction = -direction;
        return direction;
    };
    result.fontFamilies = [] {
        return QStringList{};
    };
    result.uiFont = [] {
        return ui::interfaceFont();
    };
    result.suspended = [] {
        return false;
    };
    result.lightAvailable = [] {
        return true;
    };
    // Page-turn animation: the pbink plugin sweeps the new page in, strip by strip.
    result.animationSupport = [] {
        return PageAnimationSupport::Supported;
    };
    result.cancelAnimation = [] {};
    result.light = [](QWidget *) {
        return std::make_unique<Light>();
    };
    result.dictionary = [] {
        return std::make_unique<Dictionary>();
    };
    // Kobalt's own widget keyboard (the firmware's keyboard belongs to inkview tasks, not to Qt).
    result.keyboard = ui::makeWidgetKeyboard;
    result.syncNetwork = [] {
        return std::make_unique<Network>();
    };
    result.presentation = [](QWidget *) {
        return std::make_unique<Presentation>();
    };
    return result;
}
}

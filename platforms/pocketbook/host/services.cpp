#include "services.h"
#include "host.h"
#include "sleep_screen_files.h"
#include "interfaces/reader/ui/controls/keyboard.h"
#include "interfaces/reader/ui/style/fonts.h"
#include <QGuiApplication>
#include <qpa/qplatformnativeinterface.h>
#include <QScreen>
#include <QFile>
#include <QKeyEvent>
#include <QPointer>
#include <QTime>
#include <QTimer>
#include <QElapsedTimer>
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
    bool automaticWarmth() const override {
        return false;
    }
    int setWarmth(int value) override {
        value = qBound(0, value, 100);
        pendingWarmth = value;
        schedule();
        return value;
    }
    bool saveWarmth() override {
        return true;
    }
    bool setAutomaticWarmth(bool) override {
        return false;
    }
    bool beginGesture() override {
        original = brightness();
        active = true;
        return true;
    }
    int setGestureLevel(int percent) override {
        percent = qBound(0, percent, 100);
        pendingBrightness = percent;
        schedule();
        return percent;
    }
    void finishGesture(bool save) override {
        // Only a cancelled gesture restores; a stray finish (e.g. at close) must not touch the light.
        if (active && !save) {
            pendingBrightness = -1;
            timedLight("SetFrontlightState", original, [this] { SetFrontlightState(original); });
        }
        active = false;
        writePending(); // the last value of a saved gesture goes out now
    }
    int brightness() const override {
        return qMax(0, GetFrontlightState());
    }
    bool gesturing() const override {
        return active;
    }

private:
    bool warm = false, active = false;
    int original = 0;
    int pendingBrightness = -1, pendingWarmth = -1;
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
            timedLight("SetFrontlightState", value, [value] { SetFrontlightState(value); });
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
    result.annotationExportDirectory = [] {
        return QStringLiteral("/mnt/ext1/Kobalt/Annotations");
    };
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
    result.reverseLandscapeButtons = [] {
        return false;
    };
    // The pbink plugin maps the PocketBook's page buttons to PageDown (lower) and PageUp (upper).
    // As on Kobo: turned upside down (or inverted landscape), the lower button is the physically
    // upper one, so the pair swaps with the screen orientation.
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
    // Page-turn animation: the pbink plugin sweeps the new page in with fast updates.
    result.animationsAvailable = [] {
        return true;
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

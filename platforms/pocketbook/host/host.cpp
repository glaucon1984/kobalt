#include "host.h"
#include "boot_page.h"
#include "services.h"
#include "interfaces/reader/views/opening.h"
#include "platform/api/kobo_refresh.h"
#include "interfaces/reader/views/reading_page.h"
#include "dictionaries/manager.h"
#include "sleep_screen_files.h"
#include "firmware_power.h"
#include "storage/reader_storage.h"
#include <QJsonObject>
#include <qpa/qplatformnativeinterface.h>
#include <inkview.h>
#include <unistd.h>
#include <QApplication>
#include <QCloseEvent>
#include <QDialog>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QScreen>
#include <QKeyEvent>
#include <QPointer>
#include <QDynamicPropertyChangeEvent>
#include <QLabel>
#include <QPixmap>
#include <QLineEdit>
#include <QTimer>
#include <QVBoxLayout>

namespace kobalt::platform {
void flashNextFrame();
void animateNextFrame(int direction);
}

namespace kobalt::pocketbook {
bool Host::quitting = false;
Host *Host::current = nullptr;
QElapsedTimer Host::rotationClock;

Host::Host(storage::Paths paths, EngineFontInputs fonts, QString placeholder)
    : controller(
          paths, std::move(fonts), std::move(placeholder), [] { return readerServices(); }, &Host::rotateScreen) {
    setObjectName("pocketbookHost");
    preferencesPath = paths.preferences();
    // Kobalt's font setup creates a hidden window before this one, so the platform plugin does not
    // force this window to the screen size on its own: take the whole panel explicitly.
    setGeometry(QGuiApplication::primaryScreen()->geometry());
    connect(QGuiApplication::primaryScreen(), &QScreen::geometryChanged, this, &Host::followScreen);
    // Until a book is open, Kobalt's orientation logic is not running: show the message and the
    // opening screen the way the device is held, from the gravity sensor, within what Kobalt's
    // rotation setting allows, and follow flips.
    rotateScreen(startOrientation(GetGSensorOrientation()));
    auto *native = QGuiApplication::platformNativeInterface();
    if (auto fn = native ? native->platformFunction("pbinkNotifier") : nullptr)
        if (auto *notifier = reinterpret_cast<QObject *(*)()>(fn)()) {
            connect(notifier, SIGNAL(deviceOrientation(int)), this, SLOT(deviceTurned(int)));
            connect(notifier, SIGNAL(taskBackground()), this, SLOT(taskBackground()));
            connect(notifier, SIGNAL(taskShown()), this, SLOT(taskForeground()));
            connect(notifier, SIGNAL(usbEvent(int)), this, SLOT(usbEvent(int)));
        }
    current = this;
    if (auto fn = native ? native->platformFunction("pbinkSetForegroundFilter") : nullptr)
        reinterpret_cast<void (*)(int (*)())>(fn)(&Host::quietForeground);
    powerOffWakeFallback.setSingleShot(true);
    powerOffWakeFallback.setInterval(4000);
    connect(&powerOffWakeFallback, &QTimer::timeout, this, [this] {
        if (powerOffWakeHold)
            finishWake("no power-off followed");
    });
    message = new QLabel(this);
    message->setWordWrap(true);
    message->setAlignment(Qt::AlignCenter);
    message->setMargin(48);
    addWidget(message);
    bootPage = new BootPage(this);
    addWidget(bootPage);
    openingPage = new QWidget(this);
    auto *body = new QVBoxLayout(openingPage);
    body->setContentsMargins(0, 0, 0, 0);
    opening = createEmbeddedOpening(openingPage, readerServices().presentation);
    body->addWidget(opening->window());
    connect(opening->window(), &QDialog::rejected, this, &QWidget::close);
    addWidget(openingPage);

    connect(&controller, &android::ReaderController::openingProgress, this, [this](int percent) {
        if (currentWidget() == openingPage)
            opening->setProgress(percent);
        else if (currentWidget() == bootPage)
            bootPage->setProgress(percent);
    });
    connect(&controller, &android::ReaderController::openingCover, this, [this](QImage cover) {
        bookCover = cover;
        if (currentWidget() == openingPage)
            opening->setCover(std::move(cover));
    });
    connect(&controller, &android::ReaderController::opened, this, [this] {
        openingBusy = false;
        updateKeepAwake();
        auto *reader = controller.window();
        // Dictionary downloads run without input: keep the device awake while the manager is busy.
        if (auto *dictionaries = reader->findChild<DictionaryManager *>()) {
            connect(dictionaries, &DictionaryManager::changed, this, [this, dictionaries] {
                const bool busy = dictionaries->busy();
                if (busy != downloadBusy) {
                    downloadBusy = busy;
                    updateKeepAwake();
                }
            });
        }
        addWidget(reader);
        reader->resize(size());
        setCurrentWidget(reader);
        reader->show();
        reader->setFocus();
        platform::setReaderRefreshWindow(reader);
        reader->checkpoint();
        qInfo("kobalt-pb: book opened");
        markCurrentBook(bookPath, QFileInfo(bookPath).completeBaseName());
        // Lock image for the first standby of this book (cover decode is asynchronous: give it a
        // moment, and try again later in case it took longer).
        holdAwakeForRefresh();
        QTimer::singleShot(3000, this, &Host::primeSleepScreen);
    });
    connect(&controller, &android::ReaderController::failed, this, [this](const QString &error) {
        openingBusy = false;
        updateKeepAwake();
        qWarning("kobalt-pb: opening failed: %s", qPrintable(error));
        showMessage(error + "\n\n" + tr("Tap or press a key to leave."));
    });
    // The reader is going away: nothing of this host must be painted any more (the opening page
    // would flash, rotated back to the start orientation by Kobalt's orientation restore).
    connect(&controller, &android::ReaderController::retiring, this, [this] {
        downloadBusy = openingBusy = false;
        updateKeepAwake();
        quitting = true;
        setUpdatesEnabled(false);
        markCurrentBook({});
    });
    connect(&controller, &android::ReaderController::closed, this, [this] {
        qInfo("kobalt-pb: book closed");
        quitting = true;
        setUpdatesEnabled(false);
        markCurrentBook({});
        // Standby and power-off from the library are the firmware's business: its own screens
        // from here on, whatever Kobalt showed while the book was open.
        leaveFirmwareScreens(bookCover, "close");
        if (closing)
            close();
        else
            qApp->quit();
    });
    qApp->installEventFilter(this);
    inputSettle.setSingleShot(true);
    inputSettle.setInterval(2000);
    connect(&inputSettle, &QTimer::timeout, this, [this] {
        inputBusy = false;
        updateKeepAwake();
    });
    sleepRefreshTimer.setSingleShot(true);
    sleepRefreshTimer.setInterval(300);
    connect(&sleepRefreshTimer, &QTimer::timeout, this, [this] { refreshSleepScreen("reading"); });
    sleepRefreshBusyTimeout.setSingleShot(true);
    sleepRefreshBusyTimeout.setInterval(20000);
    connect(&sleepRefreshBusyTimeout, &QTimer::timeout, this, [this] {
        if (sleepRefreshHolds) {
            qWarning("kobalt-pb: refresh keep-awake released by timeout (%d holds)", sleepRefreshHolds);
            sleepRefreshHolds = 0;
            updateKeepAwake();
        }
    });
    // Diagnostics: a synced line every 30 s of awake time, to tell a hung task from a lost log tail.
    heartbeat.setInterval(30000);
    connect(&heartbeat, &QTimer::timeout, this, [this] { qInfo("kobalt-pb: alive%s", asleep ? " (asleep)" : ""); });
    heartbeat.start();
    syncPoll.setInterval(200);
    connect(&syncPoll, &QTimer::timeout, this, [this] {
        auto *reader = controller.window();
        const bool pending = reader && !quitting && reader->pendingNetworkSync();
        // Up to 6 s: the connection itself now runs in the background and takes 2-3 s when the
        // network is there, before the upload's own 2 s.
        if (pending && syncClock.elapsed() < 6000)
            return;
        syncPoll.stop();
        qInfo("kobalt-pb: standby sync %s after %lld ms", pending ? "still pending, releasing" : "done",
              static_cast<long long>(syncClock.elapsed()));
        syncBusy = false;
        updateKeepAwake();
    });
}

Host::~Host() {
    qApp->removeEventFilter(this);
    if (current == this)
        current = nullptr;
}

void Host::start() {
    if (started)
        return;
    started = true;
    // A power-off (timed, or by hand from the lock screen) while Kobalt had the light off for
    // the standby: the firmware keeps the light off across it. Normally switched back on at
    // launch already (main); this catches a record that launch could not act on.
    restoreStandbyLight("start");
    // The page to show is chosen before the window is shown: the first frame is the boot
    // capture (or the opening view), never the empty host while the capture is read.
    if (!startPath.isEmpty())
        open(startPath);
    else
        showMessage(startMessage);
    showFullScreen();
}

void Host::open(const QString &path) {
    const QFileInfo file(path);
    if (!file.isFile() || !file.isReadable()) {
        showMessage(tr("Cannot read %1").arg(path) + "\n\n" + tr("Tap or press a key to leave."));
        return;
    }
    bookPath = file.absoluteFilePath();
    // Resumed after a power-off (the firmware reopens the book at boot, or the user does): the
    // page the boot logo shows, instead of the opening view, until the live page replaces it,
    // so the boot reads as one continuous page. The capture comes back as logical content and
    // the screen is already turned the way the device is held (the constructor read the gravity
    // sensor), so a page captured upside down and resumed upright reads right, and vice versa.
    // A book that was closed on purpose has no capture any more (markCurrentBook) and gets
    // Kobalt's opening view as usual; a different book never matches the capture's path, and a
    // capture taken in this same boot (no power-off since) is not used either. Nor is one at an
    // open long after the boot: the capture stands in for the page the boot screen just showed,
    // which is the firmware's relaunch of the book (about 15 s in) or an open from the home
    // screen right after; a book opened later in the day (after a PC session, say) starts with
    // the opening view, whatever ended the session before.
    static const int captureWindowSeconds = 180;
    const double uptime = uptimeSeconds();
    QImage saved;
    if (uptime >= 0 && uptime <= captureWindowSeconds) {
        saved = savedBootLogo(bookPath);
    } else {
        forgetBootLogo();
        qInfo("kobalt-pb: boot capture not used: opened %.0f s after the boot", uptime);
    }
    if (!saved.isNull() && saved.size() == size()) {
        bootPage->setPage(saved);
        setCurrentWidget(bootPage);
        qInfo("kobalt-pb: showing the boot logo's page with the loading bar while the book loads (uptime %.0f s)",
              uptimeSeconds());
    } else {
        setCurrentWidget(openingPage);
    }
    openingBusy = true;
    updateKeepAwake();
    controller.open(bookPath, size());
}

double Host::uptimeSeconds() {
    QFile file(QStringLiteral("/proc/uptime"));
    if (!file.open(QIODevice::ReadOnly))
        return -1;
    bool ok = false;
    const double seconds = QString::fromLatin1(file.readLine()).section(' ', 0, 0).toDouble(&ok);
    return ok ? seconds : -1;
}

QImage Host::capturePanel() {
    auto *native = QGuiApplication::platformNativeInterface();
    if (auto fn = native ? native->platformFunction("pbinkPanelImage") : nullptr)
        return reinterpret_cast<QImage (*)()>(fn)();
    return {};
}

// Kobalt's own sleep sequence, run while the device is already behind the lock screen (or at
// open, invisibly): it saves progress and composes the sleep image, which the presentation
// writes as the firmware's lock bitmap for the next standby.
void Host::taskBackground() {
    if (powerOffWakeHold) {
        // The timed power-off's own background event: the task never woke, the images written
        // at the standby stand, and the firmware shows the power-off logo next. The light stays
        // off, and so the firmware saves it (it saves what its monitor last applied; a write
        // into the shared light state without the message was not saved): the next launch
        // switches it on (main).
        powerOffWakeHold = false;
        powerOffWakeFallback.stop();
        updateKeepAwake();
        qInfo("kobalt-pb: timed power-off follows the wake, as expected");
        return;
    }
    auto *reader = controller.window();
    if (!reader || quitting || asleep || currentWidget() != reader)
        return;
    asleep = true;
    // A USB cable to a PC arrives as this same event (the firmware's PC Link / Charge prompt
    // takes the foreground); PC Link then takes the storage away and ends the app without a
    // close. Such a session is not resumed from the boot capture: the next open gets the
    // opening view. Told apart from a standby on the charger by the connection being new.
    const bool usbPlug = IsUSBconnected() > 0 && !usbAtLastInput;
    // The page as it was shown, before the sleep view is painted over it: the boot logo.
    const QImage page = usbPlug ? QImage() : capturePanel();
    if (sleepRefreshTimer.isActive()) {
        sleepRefreshTimer.stop();
        releaseAwakeForRefresh();
    }
    if (lockImageReloadInFlight())
        qWarning("kobalt-pb: standby while a lock image reload is in flight (half-asleep risk)");
    // sleepReader() composes the sleep image only while the reader reports itself ready, which
    // includes updatesEnabled(); disable painting only afterwards, before the hide.
    reader->sleepReader();
    setUpdatesEnabled(false);
    reader->sleepViewReady();
    // KOsync's final upload runs after the sleep view (the reader bounds it to 2 s); a suspended
    // CPU would lose it, so stay awake until the reader reports it done.
    if (reader->pendingNetworkSync()) {
        syncBusy = true;
        updateKeepAwake();
        syncClock.start();
        syncPoll.start();
    }
    // A power-off usually follows a standby (long press on the lock screen): write the power-off
    // image for the sleep screen just presented (its page or cover under the "Powered off"
    // status, so the two screens tell apart as on Kobo), not an older one, and the boot logo
    // with it. The reader reuses its sleep image for it, so this is a tile paint and a write.
    // With the built-in sleep screen the power-off file holds the plain cover (applyStockSleepScreen),
    // as the firmware's reader leaves it; the boot capture is kept in every mode, so a power-off
    // with the book open boots into the page whatever the sleep screen.
    if (usbPlug) {
        // PC Link ends the app under the firmware's prompt, without a close: from here on the
        // book is open in Kobalt no longer, so a standby or power-off from the home screen is
        // the firmware's, with its own screens (as at a close). Should the prompt end in
        // Charge instead, Kobalt is woken with the book still open and writes its sleep image
        // again (finishWake). The writes land before the sync below.
        leaveFirmwareScreens(bookCover, "USB");
        firmwareScreensLeft = true;
        qInfo("kobalt-pb: USB connected: firmware screens restored, boot capture forgotten");
    } else {
        if (sleepMode() != sleep::Mode::Stock)
            writePowerOffImage(reader->composePowerOffImage());
        updateBootLogo(page, "standby");
    }
    // EVT_BACKGROUND precedes every standby and every USB export. The firmware's PC Link export
    // was seen to drop what the apps had written since the last sync(2) (a standby's suspend
    // syncs, fdatasync alone did not survive), so flush everything now; ~0.1-0.3 s.
    ::sync();
    // The firmware lights the panel again at every resume, including the one its timed
    // power-off causes from standby, which lit the screen for a few seconds on the way to off.
    // The lock itself switches the light off before this runs (seen: the flag reads off here),
    // so this is usually nothing; when it is not, a wake by a press switches the light on again
    // (finishWake) and the power-off wake leaves it off (quietForeground). Not for a PC plug,
    // which ends the app under the firmware's prompt.
    standbyLightOff = !usbPlug && darkenForStandby() >= 0;
    qInfo("kobalt-pb: sleep sequence done (standby)%s", standbyLightOff ? ", frontlight off" : "");
}

// Asked by the platform plugin at EVT_FOREGROUND, before it repaints the panel: a wake that
// the firmware's timed power-off caused (its timer is due) is not shown at all. The timer then
// fires in this process within about 1.5 s and the firmware takes over; should it not (a press
// in the last seconds before it was due), the fallback timer or the press itself wakes the task.
int Host::quietForeground() {
    Host *self = current;
    if (!self || quitting || !self->asleep || self->powerOffWakeHold)
        return 0;
    const int due = secondsToTimedPowerOff();
    self->powerOffWakeExpected = due != INT_MIN && due <= 15;
    if (due != INT_MIN)
        qInfo("kobalt-pb: wake with the timed power-off due in %d s%s", due,
              self->powerOffWakeExpected ? ": staying asleep" : "");
    return self->powerOffWakeExpected ? 1 : 0;
}

void Host::taskForeground() {
    if (!asleep)
        return;
    if (powerOffWakeExpected) {
        powerOffWakeExpected = false;
        powerOffWakeHold = true;
        updateKeepAwake();
        powerOffWakeFallback.start();
        // The firmware lights the panel at the resume (seen: on at the level it had before the
        // standby, which the lock itself had switched off): off again, and recorded, since the
        // firmware keeps this off state across the power-off and the next start restores it.
        qInfo("kobalt-pb: power-off wake: frontlight %s at %d, kept dark", frontlightEnabled() ? "on" : "off",
              frontlightLevel());
        if (darkenForStandby() >= 0)
            standbyLightOff = true;
        return;
    }
    finishWake("press");
}

void Host::finishWake(const char *how) {
    const bool held = powerOffWakeHold;
    powerOffWakeHold = false;
    powerOffWakeFallback.stop();
    asleep = false;
    if (standbyLightOff) {
        standbyLightOff = false;
        restoreStandbyLight(how);
    }
    qInfo("kobalt-pb: wake (%s)", how);
    if (held) {
        // The plugin left the panel as the firmware had it: paint everything now.
        updateKeepAwake();
        auto *native = QGuiApplication::platformNativeInterface();
        if (auto fn = native ? native->platformFunction("pbinkFullRefresh") : nullptr)
            reinterpret_cast<void (*)()>(fn)();
    }
    ::sync(); // cheap here, and what is written before a plug must be on the flash (see taskBackground)
    auto *reader = controller.window();
    if (reader && !quitting) {
        reader->prepareWake();
        reader->wakeReader();
        reader->show();
    }
    setUpdatesEnabled(true);
    if (firmwareScreensLeft) {
        // The USB prompt ended in Charge: the book is still open here, so Kobalt's sleep image
        // goes back (the files were removed at the plug) before the next standby.
        firmwareScreensLeft = false;
        if (reader && !quitting) {
            qInfo("kobalt-pb: awake after the USB prompt with the book open: sleep image written again");
            scheduleSleepRefresh();
        }
    }
    if (takeLockReloadPending()) {
        // Right after the unlock press, when another lock is least likely.
        holdAwakeForRefresh();
        QPointer<Host> self(this);
        QTimer::singleShot(300, this, [self] {
            requestLockImageReload("wake", [self] {
                if (self)
                    self->releaseAwakeForRefresh();
            });
        });
    }
}

// The first lock image of the book, through the same burst-aware scheduler as a page turn: the
// composition and the bitmap write stay out of the way of pages turned right after the open
// (the engine is still rendering neighbours then, and a frame blitted under a late update tore).
// The open's own keep-awake hold is handed over to the scheduler's.
void Host::primeSleepScreen() {
    releaseAwakeForRefresh();
    scheduleSleepRefresh();
}

// Kobalt's sleep image for the current page, written as the lock bitmap and reloaded by the
// task manager while the device is awake, so that the next standby shows the page being read
// and not the one before it. Skipped when nothing changed (same progress, same battery).
// A page paint or a page-turn property change restarts the 1.2 s debounce; paints caused by the
// composition itself (page snapshot) are ignored.
// The refresh follows the page turn by 0.3 s, so that the bitmap write and the task manager's
// reload (together well under a second) are over while the hand is still on the page button:
// a lock that lands while taskmgr reloads leaves the device half asleep, and nothing the app can
// do refuses a lock (iv_sleepmode(0) does not). Quick successive turns coalesce into one refresh.
void Host::scheduleSleepRefresh() {
    if (asleep || quitting || composingSleepImage || currentWidget() != controller.window())
        return;
    sleepRefreshRetries = 0;
    if (!sleepRefreshTimer.isActive())
        holdAwakeForRefresh();
    // A turn within a second of the previous one is browsing: composing (tens of ms even with
    // the cached cover) would slow the next turn, so the refresh waits for the burst to end.
    const bool burst = lastPagePaint.isValid() && lastPagePaint.elapsed() < 1000;
    lastPagePaint.start();
    sleepRefreshTimer.start(burst ? 3000 : 300);
}

void Host::holdAwakeForRefresh() {
    ++sleepRefreshHolds;
    sleepRefreshBusyTimeout.start(); // safety net: never hold the device awake for long
    updateKeepAwake();
}

void Host::releaseAwakeForRefresh() {
    if (sleepRefreshHolds > 0)
        --sleepRefreshHolds;
    if (!sleepRefreshHolds)
        sleepRefreshBusyTimeout.stop();
    updateKeepAwake();
}

void Host::refreshSleepScreen(const char *reason) {
    auto *reader = controller.window();
    if (!reader || quitting || asleep || currentWidget() != reader) {
        releaseAwakeForRefresh();
        return;
    }
    // The built-in sleep screen: the reader composes nothing for it (captureSleepScreen), and
    // the firmware's own picture is put back instead of Kobalt's last one staying on disk.
    if (sleepMode() == sleep::Mode::Stock) {
        applyStockSleepScreen();
        releaseAwakeForRefresh();
        return;
    }
    stockApplied = false;
    composingSleepImage = true;
    QElapsedTimer composeClock;
    composeClock.start();
    QImage powerOff;
    const QImage image = reader->composeSleepImage(&powerOff);
    composingSleepImage = false;
    if (composeClock.elapsed() > 30)
        qInfo("kobalt-pb: sleep image composed in %lld ms", static_cast<long long>(composeClock.elapsed()));
    if (image.isNull()) {
        // Not ready (a passage jump still renders its neighbours, a panel is open): try again.
        if (++sleepRefreshRetries <= 5) {
            qInfo("kobalt-pb: sleep image not ready (%s), retry %d", reason, sleepRefreshRetries);
            sleepRefreshTimer.start(2000);
        } else {
            releaseAwakeForRefresh();
        }
        return;
    }
    sleepRefreshRetries = 0;
    QPointer<Host> self(this);
    const QString why = QString::fromLatin1(reason);
    const bool queued = writeLockImagesAsync(image, powerOff, [self, why](bool ok) {
        if (!self || quitting)
            return;
        if (ok && !self->bootLogoForBook && !self->asleep) {
            self->bootLogoForBook = true;
            self->updateBootLogo(capturePanel(), "open");
        }
        if (!ok || self->asleep) {
            if (ok)
                markLockReloadPending();
            self->releaseAwakeForRefresh();
            return;
        }
        requestLockImageReload(why.toLatin1().constData(), [self] {
            if (self)
                self->releaseAwakeForRefresh();
        });
    });
    if (!queued) // same image as the one already on disk
        releaseAwakeForRefresh();
}

sleep::Mode Host::sleepMode() const {
    return sleep::mode(storage::readPreferences(preferencesPath).object);
}

// Once per choice of the built-in sleep screen: Kobalt's lock bitmaps go, taskmgr reloads (so
// the next standby shows the firmware's picture, not its cached copy of Kobalt's), and the
// power-off file gets the book's plain cover, which is what the firmware's own reader puts
// there: the firmware's Power-off logo setting then means the same with Kobalt as with its
// reader (the cover, or the firmware's default or the user's own pictures, which ignore the
// file). The boot capture is not the sleep screen's business: it is taken here after the open
// (the lock-image write, which triggers it in the other modes, does not happen) and at every
// standby, so a power-off with the book open boots into the page in this mode too.
void Host::applyStockSleepScreen() {
    if (stockApplied)
        return;
    stockApplied = true;
    if (!writeCoverAsPowerOffImage(bookCover))
        qInfo("kobalt-pb: built-in sleep screen: no cover for the power-off file");
    if (!bootLogoForBook) {
        bootLogoForBook = true;
        updateBootLogo(capturePanel(), "open");
    }
    if (!restoreFirmwareLockImages()) {
        qInfo("kobalt-pb: built-in sleep screen: no lock image of Kobalt's to remove");
        return;
    }
    holdAwakeForRefresh();
    QPointer<Host> self(this);
    requestLockImageReload("built-in sleep screen", [self] {
        if (self)
            self->releaseAwakeForRefresh();
    });
}

void Host::usbEvent(int type) {
    // Seen on the Era Lite: a USB plug reaches the app as EVT_BACKGROUND (the firmware's PC Link /
    // Charge prompt takes the foreground), so the standby sequence above saves progress; the
    // storage events below were never delivered to the task. Kept in case other firmware does.
    // 163 = USB storage connected: save now. 72 = file system about to go away (PC Link chosen).
    qInfo("kobalt-pb: usb event %d", type);
    if (type == 163) {
        if (auto *reader = controller.window())
            reader->checkpoint();
        ::sync(); // PC Link may take the storage away without a flush
    } else if (type == 72) {
        qInfo("kobalt-pb: storage leaving, closing");
        close();
    }
}

void Host::updateKeepAwake() {
    keepAwake(openingBusy || downloadBusy || syncBusy || bootLogoBusy || inputBusy || powerOffWakeHold ||
              sleepRefreshHolds > 0);
}

void Host::updateBootLogo(const QImage &panel, const char *reason) {
    if (quitting || bootLogoWriteInFlight())
        return;
    QPointer<Host> self(this);
    const bool started = writeBootLogo(panel, bookPath, reason, [self] {
        if (!self)
            return;
        self->bootLogoBusy = false;
        self->updateKeepAwake();
    });
    if (started) {
        bootLogoBusy = true;
        updateKeepAwake();
    }
}

bool Host::connectNetwork(bool interactive) {
    auto *native = QGuiApplication::platformNativeInterface();
    if (auto fn = native ? native->platformFunction("pbinkConnectNetwork") : nullptr)
        return reinterpret_cast<int (*)(int)>(fn)(interactive ? 1 : 0) == 1;
    return false;
}

void Host::keepAwake(bool keep) {
    auto *native = QGuiApplication::platformNativeInterface();
    if (auto fn = native ? native->platformFunction("pbinkSetKeepAwake") : nullptr)
        reinterpret_cast<void (*)(int)>(fn)(keep ? 1 : 0);
}

void Host::followScreen() {
    const QRect screen = QGuiApplication::primaryScreen()->geometry();
    setGeometry(screen);
    if (auto *reader = controller.window())
        reader->resize(screen.size());
    qInfo("kobalt-pb: screen now %dx%d", screen.width(), screen.height());
}

Qt::ScreenOrientation Host::fromInkviewOrientation(int value) {
    switch (value) {
    case 3: return Qt::InvertedPortraitOrientation;
    case 2: return Qt::LandscapeOrientation;
    case 1: return Qt::InvertedLandscapeOrientation;
    default: return Qt::PortraitOrientation;
    }
}

Qt::ScreenOrientation Host::startOrientation(int inkviewOrientation) const {
    const Qt::ScreenOrientation held = fromInkviewOrientation(inkviewOrientation);
    const QJsonObject settings = storage::readPreferences(preferencesPath).object;
    if (settings.value("auto_rotate").toBool(false))
        return held;
    const int saved = settings.value("orientation").toInt(Qt::PortraitOrientation);
    const Qt::ScreenOrientation axis =
        saved == Qt::LandscapeOrientation || saved == Qt::InvertedLandscapeOrientation ? Qt::LandscapeOrientation
                                                                                        : Qt::PortraitOrientation;
    const bool onAxis = axis == Qt::PortraitOrientation
                            ? (held == Qt::PortraitOrientation || held == Qt::InvertedPortraitOrientation)
                            : (held == Qt::LandscapeOrientation || held == Qt::InvertedLandscapeOrientation);
    return onAxis ? held : axis;
}

void Host::deviceTurned(int inkviewOrientation) {
    if (controller.window())
        return;
    // Like Kobalt: settle for 250 ms, transient readings while turning must not flip the screen.
    pendingTurn = inkviewOrientation;
    if (!turnTimer.isActive()) {
        turnTimer.setSingleShot(true);
        connect(&turnTimer, &QTimer::timeout, this, [this] {
            if (!controller.window() && pendingTurn >= 0)
                rotateScreen(startOrientation(pendingTurn));
        }, Qt::UniqueConnection);
    }
    turnTimer.start(250);
}

void Host::rotateScreen(Qt::ScreenOrientation orientation) {
    if (quitting) {
        qInfo("kobalt-pb: ignoring rotation to %d while quitting", int(orientation));
        return;
    }
    qInfo("kobalt-pb: rotate to Qt orientation %d", int(orientation));
    auto *native = QGuiApplication::platformNativeInterface();
    if (auto fn = native ? native->platformFunction("pbinkSetOrientation") : nullptr) {
        const auto sideways = [](Qt::ScreenOrientation value) {
            return value == Qt::LandscapeOrientation || value == Qt::InvertedLandscapeOrientation;
        };
        const auto before = QGuiApplication::primaryScreen()->orientation();
        reinterpret_cast<void (*)(int)>(fn)(int(orientation));
        // A half turn keeps the layout: the plugin's full refresh already drew the final page, and
        // no reflow follows, so waiting for one flashed the next page turn instead.
        if (before == orientation || sideways(before) != sideways(orientation))
            rotationClock.start();
    } else {
        qWarning("kobalt-pb: no rotation support in the platform plugin");
    }
}

// Kobo's panel flashes by itself when the screen turns; this one does not, and the reflowed
// page would be drawn over the "updating page layout" notice with a partial update. The page
// repaints under the notice while the engine is busy (seen 1 s after a turn), so time is no
// guide: the first page paint for which the reader reports itself ready (reflow done, page image
// at the new viewport size) is the reflowed page, and that frame gets the full flash. Forgotten
// after a minute, in case no such paint comes (menu opened, book closed).
// The same for the page under a closed KOsync dialog (the opening notice, the position choice,
// the manual-sync prompt): Kobalt hides them without a flash request and the page came back with
// a partial update, keeping the card's ghost. For "Use remote" the first ready paint is the
// destination page, after the jump. Forgotten after 10 s.
void Host::flashAfterTurn() {
    if (rotationClock.isValid() && rotationClock.elapsed() >= 60000)
        rotationClock.invalidate();
    if (dialogClock.isValid() && dialogClock.elapsed() >= 10000)
        dialogClock.invalidate();
    if (!rotationClock.isValid() && !dialogClock.isValid())
        return;
    auto *reader = controller.window();
    if (!reader || !reader->readyForPresentation())
        return;
    if (rotationClock.isValid())
        qInfo("kobalt-pb: full flash for the reflowed page, %lld ms after the turn",
              static_cast<long long>(rotationClock.elapsed()));
    if (dialogClock.isValid())
        qInfo("kobalt-pb: full flash for the page under the closed KOsync dialog, %lld ms after it hid",
              static_cast<long long>(dialogClock.elapsed()));
    rotationClock.invalidate();
    dialogClock.invalidate();
    platform::flashNextFrame();
}

// The reader opens its menu from a tap in the centre zone of the page (its tap zones decide);
// the PocketBook's power/menu buttons get the same treatment.
void Host::openReaderMenu() {
    auto *reader = controller.window();
    if (!reader || currentWidget() != reader || QApplication::activeModalWidget())
        return;
    auto *page = static_cast<ReadingPage *>(reader->findChild<QWidget *>("rrPage"));
    if (page && page->isVisible() && page->tapped)
        page->tapped(page->rect().center());
}

void Host::showMessage(const QString &text) {
    message->setText(text);
    setCurrentWidget(message);
}

void Host::closeEvent(QCloseEvent *event) {
    if (controller.idle()) {
        event->accept();
        return;
    }
    event->ignore();
    if (closing)
        return;
    closing = true;
    controller.close();
}

bool Host::eventFilter(QObject *object, QEvent *event) {
    // Kobalt's dictionaries page downloads from GitHub: bring Wi-Fi up as it opens (the firmware
    // keeps Wi-Fi off until something asks for it).
    if (event->type() == QEvent::Show && object->objectName() == "rrDictionaries")
        QTimer::singleShot(0, this, [] { ensureNetwork(); });
    // A KOsync dialog closing (the opening notice with the sync result, the position choice, the
    // manual-sync prompt): the next page paint with the reader ready gets a full flash
    // (flashAfterTurn), the card would otherwise leave its ghost under the page.
    if (event->type() == QEvent::Hide) {
        const auto name = object->objectName();
        if (name == "rrKosyncOpening" || name == "rrKosyncChoice" || name == "rrKosyncManualPrompt")
            dialogClock.start();
    }
    // Kobalt's full-refresh request (as the Kobo flush hook and the Android canvas consume it).
    if (event->type() == QEvent::DynamicPropertyChange) {
        auto *change = static_cast<QDynamicPropertyChangeEvent *>(event);
        if (change->propertyName() == "reader_full_flash" && object->property("reader_full_flash").toBool()) {
            object->setProperty("reader_full_flash", false);
            platform::flashNextFrame();
        } else if (change->propertyName() == "reader_animation_direction") {
            const int direction = object->property("reader_animation_direction").toInt();
            if (direction) {
                object->setProperty("reader_animation_direction", 0);
                platform::animateNextFrame(direction);
            }
        } else if (change->propertyName() == "reader_page_turn") {
            scheduleSleepRefresh();
        } else if (change->propertyName() == "reader_exit_clean_pending" &&
                   object->property("reader_exit_clean_pending").toBool()) {
            // Kobalt is closing the book on its own: the restore of the start orientation follows,
            // nothing of this host may be painted any more.
            quitting = true;
            setUpdatesEnabled(false);
        }
        return false;
    }
    if (powerOffWakeHold && (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::KeyPress))
        finishWake("input"); // a press after all, not the timed power-off
    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease) {
        if (event->type() == QEvent::MouseButtonPress) {
            usbAtLastInput = IsUSBconnected() > 0;
            inputSettle.stop();
            if (!inputBusy) {
                inputBusy = true;
                updateKeepAwake();
            }
        } else {
            inputSettle.start();
        }
    }
    if (event->type() == QEvent::Paint && object->objectName() == "rrPage") {
        flashAfterTurn();
        scheduleSleepRefresh();
    }
    if (currentWidget() == message &&
        (event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::KeyPress)) {
        close();
        return true;
    }
    if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (event->type() == QEvent::KeyPress)
            usbAtLastInput = IsUSBconnected() > 0;
        if (key->key() == Qt::Key_Home || key->key() == Qt::Key_Back) {
            if (event->type() == QEvent::KeyPress)
                close();
            return true;
        }
        if (key->key() == Qt::Key_PowerOff || key->key() == Qt::Key_Menu) {
            if (event->type() == QEvent::KeyPress && !key->isAutoRepeat())
                openReaderMenu();
            return true;
        }
        // The reader only handles page keys itself while it is the active modal widget (as on
        // Kobo); here it is a child of this host, so forward the physical page buttons.
        auto *reader = controller.window();
        if (reader && currentWidget() == reader && reader->isEnabled() && !QApplication::activeModalWidget() &&
            !qobject_cast<QLineEdit *>(QApplication::focusWidget())) {
            const int direction = readerServices().pageButtonDirection(key);
            if (direction) {
                if (event->type() == QEvent::KeyPress) {
                    // A held button keeps turning, as in the firmware's reader and KOReader:
                    // inkview repeats the key while it is down (EVT_KEYREPEAT, Qt auto-repeat
                    // here). One turn per repeat, once the previous page is on screen and no
                    // sooner than the plugin's frame pacing, so turns do not pile up.
                    if (!key->isAutoRepeat()) {
                        reader->pageButton(direction);
                        heldTurnClock.start();
                    } else if (reader->readyForPresentation() && heldTurnClock.isValid() &&
                               heldTurnClock.elapsed() >= 350) {
                        reader->pageButton(direction);
                        heldTurnClock.start();
                    }
                }
                return true;
            }
        }
    }
    return false;
}
}

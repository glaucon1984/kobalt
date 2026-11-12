#pragma once
#include "platforms/standalone/reader.h"
#include "reading/sleep_screen.h"
#include <QStackedWidget>
#include <QElapsedTimer>
#include <QHash>
#include <QString>
#include <QTimer>
#include <climits>
#include <functional>
#include <memory>

class QLabel;

namespace kobalt {
class ReaderOpeningView;
}

namespace kobalt::pocketbook {
class BootPage;
// Full-screen host: Kobalt's opening view while a book loads, then the reader window itself.
// Closing the book (its own close, or the Home/Back key) quits the app, which returns the
// PocketBook to its library.
class Host final : public QStackedWidget {
    Q_OBJECT
public:
    Host(storage::Paths paths, EngineFontInputs fonts, QString placeholder);
    ~Host() override;
    void open(const QString &path);
    void showMessage(const QString &text);
    // Shown once inkview made the task visible (or after a fallback delay), so that the first
    // frame is painted after the firmware applied its orientation.
    QString startPath, startMessage;
    // Kobalt asks the host to show this orientation; the pbink plugin rotates the panel.
    static void rotateScreen(Qt::ScreenOrientation orientation);
    // inkview gravity code (0 portrait, 3 upside down, 2 clockwise, 1 counter-clockwise) to Qt.
    static Qt::ScreenOrientation fromInkviewOrientation(int value);
    // The orientation to show for a gravity reading before the book is open, under Kobalt's own
    // rotation setting (reader-settings.json): "both" follows the reading; a locked axis keeps
    // the reading only when it lies on that axis (upright or upside down within it, as
    // ReaderOrientation::permits does with a sensor), else the axis's upright orientation.
    Qt::ScreenOrientation startOrientation(int inkviewOrientation) const;
    QString preferencesPath;
    // Kobalt's profile, for the highlights mirrored into the firmware's Notes (notes_mirror.h).
    QString profileDirectory;
    // The open book's highlights by start position, with the page Kobalt shows them on (zero-
    // based, current layout): asked of the reader's engine while it is there, at standby and when
    // the reader is told to close, for the mirror that runs then or once the book is closed.
    QHash<QString, int> notePages;
    QElapsedTimer notePagesAsked;
    // Awake from a standby until the pages have come back and the mirror has run.
    bool notesBusy = false;
    void collectNotePages(std::function<void()> then);
    // Started by rotateScreen(); the first page paint with the reader ready gets a full flash.
    static QElapsedTimer rotationClock;
    // Started when a KOsync dialog hides; the page paint that restores the page gets a full flash.
    QElapsedTimer dialogClock;
    void flashAfterTurn();
    // Last page turn from a physical button; paces the turns of a held button.
    QElapsedTimer heldTurnClock;
    // No device light sleep while the engine opens a book (the firmware would suspend mid-load).
    static void keepAwake(bool keep);
    void updateKeepAwake();
    // Wi-Fi up through the firmware when a feature needs the network (dictionaries page, KOsync).
    // Blocking; interactive lets the firmware show its network dialog, else known networks only.
    static bool connectNetwork(bool interactive);
    static bool ensureNetwork() {
        return connectNetwork(true);
    }
    // sleepRefreshHolds: awake from a page turn until its lock image is on disk and reloaded
    // (the device light-sleeps within seconds of the last input and a suspended CPU runs no Qt
    // timers). Counted: the page-turn chain and the wake reload hold it independently.
    bool openingBusy = false, downloadBusy = false, syncBusy = false, bootLogoBusy = false;
    int sleepRefreshHolds = 0;
    // The firmware suspends the CPU well under a second after a touch ends while the app allows
    // it, and Qt timers sleep with it: the light notice's hide timer (850 ms) never fired and
    // the notice stayed until the next tap. Awake from a touch until 2 s after its release.
    bool inputBusy = false;
    QTimer inputSettle;
    // USB state at the last touch or key: a USB connection that is new at EVT_BACKGROUND is a PC
    // plug (the firmware's PC Link / Charge prompt took the foreground), not a standby.
    bool usbAtLastInput = false;
    // The firmware's screens were put back at a USB plug (leaveFirmwareScreens): a wake with the
    // book still open (Charge chosen) writes Kobalt's sleep image again.
    bool firmwareScreensLeft = false;
    // The frontlight was on at standby and Kobalt switched it off (the firmware's lock screen
    // is drawn by then), so that the wake the firmware's timed power-off causes stays dark; a
    // wake by a press switches it on again.
    bool standbyLightOff = false;
    // A wake the firmware's timed power-off is about to follow (quietForeground): the task
    // stays asleep, the panel and the light untouched, until the power-off's EVT_BACKGROUND;
    // a press or the fallback timer wakes it for real instead.
    bool powerOffWakeExpected = false, powerOffWakeHold = false;
    QTimer powerOffWakeFallback;
    // When the firmware's timed power-off is due, as it stood at the standby
    // (timedPowerOffMoment): the firmware's own figure may have moved by the wake.
    long powerOffDueAt = LONG_MIN;
    static int quietForeground();
    void finishWake(const char *how);
    // Boot logo from a capture of the panel: once after the book's first lock image, then at
    // each standby. Awake until the firmware's tool has returned.
    bool bootLogoForBook = false;
    void updateBootLogo(const QImage &panel, const char *reason);
    static QImage capturePanel();
    // Seconds since the device booted (/proc/uptime), -1 when unknown.
    static double uptimeSeconds();
    void holdAwakeForRefresh();
    void releaseAwakeForRefresh();
    // Kobalt's sleep screen setting as saved (reader-settings.json). With the built-in one
    // chosen, the lock bitmaps are removed once and the power-off file gets the plain cover
    // (applyStockSleepScreen), as the firmware's own reader leaves them; the boot capture is
    // kept in every mode.
    sleep::Mode sleepMode() const;
    void applyStockSleepScreen();
    bool stockApplied = false;
    // The book's cover as the controller decoded it at open; in built-in mode it is the
    // power-off file, as the firmware's own reader writes the cover there (shown when the
    // firmware's Power-off logo setting is the book cover).
    QImage bookCover;
    // The book's own "hide on sleep screens" setting, as the reader last reported it (asked at
    // each sleep screen refresh, at standby and when the reader is told to close; kept for the
    // close, when the reader is gone). Such a book leaves neither its cover nor a page in any
    // screen the firmware shows: Kobalt's private screen replaces the built-in sleep screen,
    // and a blank sheet stands for the cover and for the boot capture.
    bool bookPrivate = false;
    void updateBookPrivacy();
    QImage firmwareCover() const;
    QImage bootCapture() const;

protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *object, QEvent *event) override;

private:
    standalone::ReaderController controller;
    std::unique_ptr<ReaderOpeningView> opening;
    QWidget *openingPage;
    QLabel *message;
    BootPage *bootPage; // the boot logo's page, with a loading bar, while a reopened book loads
    QString bookPath;
    bool closing = false;
    QTimer turnTimer;
    static bool quitting;
    static Host *current;
    bool started = false;
    bool asleep = false;
    void primeSleepScreen();
    // Lock image rewritten a moment after the last page turn (the firmware draws the lock
    // screen from its cached copy the instant the button is pressed).
    QTimer sleepRefreshTimer, sleepRefreshBusyTimeout;
    QElapsedTimer lastPagePaint;
    void refreshSleepScreen(const char *reason);
    int sleepRefreshRetries = 0;
    bool composingSleepImage = false;
    void scheduleSleepRefresh();
    QTimer heartbeat;
    // KOsync's final upload at standby (bounded by the reader, 2 s): awake until it is done.
    QTimer syncPoll;
    QElapsedTimer syncClock;
    int pendingTurn = -1;
    void followScreen();

public slots:
    void start();

private slots:
    void deviceTurned(int inkviewOrientation);
    void taskBackground();
    void taskForeground();
    void usbEvent(int type);

private:
    void openReaderMenu();
};
}

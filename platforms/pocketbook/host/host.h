#pragma once
#include "platforms/android/reader.h"
#include <QStackedWidget>
#include <QElapsedTimer>
#include <QTimer>
#include <memory>

class QLabel;

namespace kobalt {
class ReaderOpeningView;
}

namespace kobalt::pocketbook {
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
    // Boot logo from a capture of the panel: once after the book's first lock image, then at
    // each standby. Awake until the firmware's tool has returned.
    bool bootLogoForBook = false;
    void updateBootLogo(const QImage &panel, const char *reason);
    static QImage capturePanel();
    void holdAwakeForRefresh();
    void releaseAwakeForRefresh();

protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *object, QEvent *event) override;

private:
    android::ReaderController controller;
    std::unique_ptr<ReaderOpeningView> opening;
    QWidget *openingPage;
    QLabel *message;
    QLabel *bootPage; // the boot logo's page while a reopened book loads
    QString bookPath;
    bool closing = false;
    QTimer turnTimer;
    static bool quitting;
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

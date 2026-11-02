#include "pbinkevents.h"
#include "pbinkscreen.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QLoggingCategory>
#include <qpa/qwindowsysteminterface.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <inkview.h> // has its own extern "C" guards

Q_DECLARE_LOGGING_CATEGORY(lcPbInk)

PbInkEvents *PbInkEvents::sInstance = nullptr;

PbInkEvents::PbInkEvents(PbInkScreen *screen, bool debug, QObject *parent)
    : QObject(parent), mScreen(screen), mDebug(debug)
{
    sInstance = this;
    mTimer.setInterval(10);
    mTimer.setTimerType(Qt::CoarseTimer);
    connect(&mTimer, &QTimer::timeout, this, &PbInkEvents::pump);
    // Exported by firmware 6.11 but missing from the SDK's header and library.
    mQuickLoop = reinterpret_cast<int (*)()>(dlsym(RTLD_DEFAULT, "iv_process_event_loop_quick"));
    if (!mQuickLoop)
        qWarning("pbink: no iv_process_event_loop_quick, input waits for the firmware's loop");
    mNetworkLimit.setSingleShot(true);
    mNetworkLimit.setInterval(45000);
    connect(&mNetworkLimit, &QTimer::timeout, this, [this] { networkDone(-1, "safety limit"); });
    mNetworkPoll.setInterval(500);
    connect(&mNetworkPoll, &QTimer::timeout, this, &PbInkEvents::pollNetwork);
}

// The scan's results against the configured networks. `scanned` tells whether the scan has
// produced anything yet (an empty list means it is still running).
bool PbInkEvents::knownNetworkInRange(bool *scanned)
{
    *scanned = false;
    iv_wifi_ap_list *aps = WiFiScanProcessGetResults();
    if (!aps)
        return false;
    *scanned = aps->ap_quantity > 0;
    bool found = false;
    int known = 0;
    if (char **names = EnumWirelessNetworks()) {
        for (char **name = names; *name && !found; ++name) {
            ++known;
            for (int i = 0; i < aps->ap_quantity && !found; ++i)
                found = strcmp(aps->apinfo[i].ssid, *name) == 0;
        }
    }
    if (*scanned)
        qInfo("pbink: scan: %d network(s) in range, %d configured, known one %s", aps->ap_quantity, known,
              found ? "in range" : "not in range");
    free(aps);
    return found;
}

void PbInkEvents::pollNetwork()
{
    if (!mConnecting)
        return;
    if (QueryNetwork() & NET_CONNECTED) {
        networkDone(0, "link up");
        return;
    }
    if (!mScanning)
        return;
    bool scanned = false;
    const bool known = knownNetworkInRange(&scanned);
    // The firmware's own chooser decides after about 9 s; a scan usually reports within 3-5 s.
    if (!scanned && mScanClock.elapsed() < 10000)
        return;
    mScanning = false;
    WiFiScanProcessStop();
    if (!known) {
        WiFiPower(0);
        networkDone(-2, scanned ? "no known network in range" : "scan gave nothing");
        return;
    }
    mNetAgent = new QProcess(this);
    mNetAgent->setProcessChannelMode(QProcess::ForwardedChannels); // its own lines go to the log
    connect(mNetAgent, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus) { networkDone(code, "agent exited"); });
    connect(mNetAgent, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) { networkDone(-int(error) - 100, "agent failed to run"); });
    mNetAgent->start(QStringLiteral("/ebrmain/bin/netagent"), {QStringLiteral("connect")});
    qInfo("pbink: known network in range, agent connecting");
}

void PbInkEvents::connectNetworkAsync(NetworkDone done, void *context)
{
    if (QueryNetwork() & NET_CONNECTED) {
        // Already up: answer from the event loop, as the asynchronous path would.
        QTimer::singleShot(0, this, [done, context] { done(1, context); });
        return;
    }
    mNetworkWaiters.append({done, context});
    if (mConnecting)
        return;
    mConnecting = true;
    qInfo("pbink: network %d, connecting silently in the background: scanning first", QueryNetwork());
    WiFiPower(1);
    if (WiFiScanProcessStart() != 0) {
        qInfo("pbink: the firmware's scan process did not start");
        QTimer::singleShot(0, this, [this] { networkDone(-3, "scan did not start"); });
        return;
    }
    mScanning = true;
    mScanClock.start();
    mNetworkLimit.start();
    mNetworkPoll.start();
}

void PbInkEvents::networkDone(int status, const char *how)
{
    if (!mConnecting)
        return;
    mConnecting = false;
    mNetworkLimit.stop();
    mNetworkPoll.stop();
    if (mScanning) {
        mScanning = false;
        WiFiScanProcessStop();
    }
    if (mNetAgent) {
        auto *agent = mNetAgent;
        mNetAgent = nullptr;
        agent->disconnect(this);
        if (agent->state() != QProcess::NotRunning) {
            // Link up before the agent returned, or the safety limit: let it finish on its own
            // for a moment, then stop it.
            QTimer::singleShot(5000, agent, [agent] {
                if (agent->state() != QProcess::NotRunning)
                    agent->kill();
                agent->deleteLater();
            });
        } else {
            agent->deleteLater();
        }
    }
    const bool connected = (QueryNetwork() & NET_CONNECTED) != 0;
    qInfo("pbink: background connection %s: status %d, network %d, %s", how, status, QueryNetwork(),
          connected ? "connected" : "not connected");
    const auto waiters = mNetworkWaiters;
    mNetworkWaiters.clear();
    for (const auto &waiter : waiters)
        waiter.first(connected ? 1 : 0, waiter.second);
}

PbInkEvents::~PbInkEvents()
{
    mTimer.stop();
    ClearOnExit();
    sInstance = nullptr;
}

void PbInkEvents::start()
{
    // Orientation events are only delivered with this set (KOReader does the same).
    setenv("SUPPORT_EVT_ORIENTATION", "YES", 1);
    PrepareForLoop(handlerStatic);
    mFirmwareStep.start();
    mTimer.start();
}

void PbInkEvents::pump()
{
    // On firmware 6.11 ProcessEventLoopQuick() is the same call as ProcessEventLoop(): it delivers
    // pending events to handlerStatic(), runs the firmware's timers, task messages, gravity sensor
    // and auto-sleep, then waits up to 20 ms for input. Called back to back, it held the GUI thread
    // three times in every page turn. iv_process_event_loop_quick() delivers pending events and
    // never waits, so it goes first: a press made while a wipe blocked this thread must not reach
    // the handler through the firmware's step. Around input it is all that runs, the firmware's
    // step keeps running once a second, and not at all in the time a press takes to become a frame
    // (70-90 ms), which only presses and taps start, so a long drag cannot starve the firmware.
    static const qint64 inputHoldMs = 250, firmwareStepMs = 1000, turnMs = 120;
    if (mQuickLoop) {
        mQuickLoop();
        const bool turning = mLastTurn.isValid() && !mLastTurn.hasExpired(turnMs);
        const bool active = mLastInput.isValid() && !mLastInput.hasExpired(inputHoldMs);
        if (turning || (active && !mFirmwareStep.hasExpired(firmwareStepMs)))
            return;
    }
    // A slow call is logged: this one was seen to block the GUI thread for over a minute once (a
    // book stayed on its loading screen until standby produced an event).
    QElapsedTimer clock;
    clock.start();
    ProcessEventLoopQuick();
    const qint64 ms = clock.elapsed();
    if (ms > 200)
        qWarning("pbink: inkview event pump took %lld ms", static_cast<long long>(ms));
    mFirmwareStep.start();
    // Input ends the wait above, but the event is only delivered by the next call.
    if (mQuickLoop)
        mQuickLoop();
}

int PbInkEvents::handlerStatic(int type, int par1, int par2)
{
    return sInstance ? sInstance->handle(type, par1, par2) : 0;
}

int PbInkEvents::mapKey(int k)
{
    switch (k) {
    case IV_KEY_PREV: case IV_KEY_PREV2: return Qt::Key_PageUp;
    case IV_KEY_NEXT: case IV_KEY_NEXT2: return Qt::Key_PageDown;
    case IV_KEY_LEFT: return Qt::Key_Left;
    case IV_KEY_RIGHT: return Qt::Key_Right;
    case IV_KEY_UP: return Qt::Key_Up;
    case IV_KEY_DOWN: return Qt::Key_Down;
    case IV_KEY_OK: return Qt::Key_Return;
    case IV_KEY_MENU: return Qt::Key_Menu;
    case IV_KEY_HOME: return Qt::Key_Home;
    case IV_KEY_BACK: return Qt::Key_Back;
    case IV_KEY_POWER: return Qt::Key_PowerOff;
    case IV_KEY_PLUS: return Qt::Key_Plus;
    case IV_KEY_MINUS: return Qt::Key_Minus;
    default: return Qt::Key_unknown;
    }
}

int PbInkEvents::handle(int type, int par1, int par2)
{
    if (type == EVT_FOREGROUND || type == EVT_BACKGROUND || type == EVT_SHOW || type == EVT_HIDE || type == EVT_EXIT)
        qInfo("pbink: lifecycle event %d (%d, %d)", type, par1, par2);
    else if (mDebug && type != EVT_POINTERMOVE && type != EVT_POINTERDRAG && type != EVT_MTSYNC)
        qCDebug(lcPbInk, "event %d (%d, %d)", type, par1, par2);
    switch (type) {
    case EVT_INIT:
        // Full-screen app: no firmware status panel over our framebuffer.
        SetPanelType(PANEL_DISABLED);
        break;
    case EVT_SHOW:
        mScreen->lockPortrait();
        mScreen->repaintAll();
        break;
    case EVT_FOREGROUND:
        // The firmware applies its own orientation to the task right before this event (also when
        // coming back from sleep or another task): pin portrait again, then the first frame.
        mScreen->lockPortrait();
        mScreen->repaintAll();
        emit taskShown();
        break;
    case EVT_BACKGROUND:
        emit taskBackground();
        break;
    case EVT_USBSTORE_IN:
    case EVT_USBSTORE_OUT:
    case EVT_FSINCOMING:
    case EVT_FSCHANGED:
        qInfo("pbink: storage event %d (%d, %d)", type, par1, par2);
        emit usbEvent(type);
        break;
    case EVT_EXIT:
        QCoreApplication::quit();
        break;
    case EVT_POINTERDOWN:
        mLastInput.start();
        mDown = true;
        mLast = mScreen->toLogical(QPoint(par1, par2));
        QWindowSystemInterface::handleMouseEvent(nullptr, mLast, mLast, Qt::LeftButton, Qt::LeftButton,
                                                 QEvent::MouseButtonPress);
        break;
    case EVT_POINTERMOVE:
    case EVT_POINTERDRAG:
        if (mDown) {
            mLastInput.start();
            mLast = mScreen->toLogical(QPoint(par1, par2));
            QWindowSystemInterface::handleMouseEvent(nullptr, mLast, mLast, Qt::LeftButton, Qt::NoButton,
                                                     QEvent::MouseMove);
        }
        break;
    case EVT_POINTERUP:
    case EVT_POINTERCANCEL:
        if (mDown) {
            mLastInput.start();
            mLastTurn.start();
            mDown = false;
            if (type == EVT_POINTERUP)
                mLast = mScreen->toLogical(QPoint(par1, par2));
            QWindowSystemInterface::handleMouseEvent(nullptr, mLast, mLast, Qt::NoButton, Qt::LeftButton,
                                                     QEvent::MouseButtonRelease);
        }
        break;
    case EVT_KEYPRESS:
    case EVT_KEYREPEAT:
    case EVT_KEYRELEASE:
    case EVT_KEYPRESS_EXT:
    case EVT_KEYREPEAT_EXT:
    case EVT_KEYRELEASE_EXT: {
        mLastInput.start();
        const bool release = type == EVT_KEYRELEASE || type == EVT_KEYRELEASE_EXT;
        const bool repeat = type == EVT_KEYREPEAT || type == EVT_KEYREPEAT_EXT;
        if (!release)
            mLastTurn.start();
        const int key = mapKey(par1);
        QWindowSystemInterface::handleKeyEvent(nullptr, release ? QEvent::KeyRelease : QEvent::KeyPress, key,
                                               Qt::NoModifier, QString(), repeat);
        break;
    }
    case EVT_ORIENTATION:
        // The gravity sensor's reading (0..3); not applied by itself, the app decides.
        qInfo("pbink: device orientation event %d", par1);
        mLastOrientation = par1;
        emit deviceOrientation(par1);
        break;
    default:
        break;
    }
    return 0;
}

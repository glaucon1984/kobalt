#include "pbinkevents.h"
#include "pbinkscreen.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QLoggingCategory>
#include <qpa/qwindowsysteminterface.h>
#include <stdlib.h>
#include <inkview.h> // has its own extern "C" guards

Q_DECLARE_LOGGING_CATEGORY(lcPbInk)

PbInkEvents *PbInkEvents::sInstance = nullptr;

PbInkEvents::PbInkEvents(PbInkScreen *screen, bool debug, QObject *parent)
    : QObject(parent), mScreen(screen), mDebug(debug)
{
    sInstance = this;
    mTimer.setInterval(20);
    mTimer.setTimerType(Qt::CoarseTimer);
    connect(&mTimer, &QTimer::timeout, this, &PbInkEvents::pump);
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
    mTimer.start();
}

void PbInkEvents::pump()
{
    // Delivers pending inkview events to handlerStatic() without waiting for new ones:
    // ProcessEventLoop() was seen to block the GUI thread for over a minute once (a book stayed
    // on its loading screen until standby produced an event). A slow call is logged either way.
    QElapsedTimer clock;
    clock.start();
    ProcessEventLoopQuick();
    const qint64 ms = clock.elapsed();
    if (ms > 200)
        qWarning("pbink: inkview event pump took %lld ms", static_cast<long long>(ms));
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
        mDown = true;
        mLast = mScreen->toLogical(QPoint(par1, par2));
        QWindowSystemInterface::handleMouseEvent(nullptr, mLast, mLast, Qt::LeftButton, Qt::LeftButton,
                                                 QEvent::MouseButtonPress);
        break;
    case EVT_POINTERMOVE:
    case EVT_POINTERDRAG:
        if (mDown) {
            mLast = mScreen->toLogical(QPoint(par1, par2));
            QWindowSystemInterface::handleMouseEvent(nullptr, mLast, mLast, Qt::LeftButton, Qt::NoButton,
                                                     QEvent::MouseMove);
        }
        break;
    case EVT_POINTERUP:
    case EVT_POINTERCANCEL:
        if (mDown) {
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
        const bool release = type == EVT_KEYRELEASE || type == EVT_KEYRELEASE_EXT;
        const bool repeat = type == EVT_KEYREPEAT || type == EVT_KEYREPEAT_EXT;
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

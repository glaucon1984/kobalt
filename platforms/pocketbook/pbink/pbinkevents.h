// Pumps inkview's event loop from Qt's and turns inkview events into Qt input events.
#pragma once
#include <QObject>
#include <QTimer>
#include <QPoint>

class PbInkScreen;

class PbInkEvents : public QObject
{
    Q_OBJECT
public:
    explicit PbInkEvents(PbInkScreen *screen, bool debug, QObject *parent = nullptr);
    ~PbInkEvents() override;
    void start();
    // Last gravity code delivered by inkview, or -1 before the first event.
    int lastOrientation() const { return mLastOrientation; }

signals:
    // inkview gravity sensor code: 0 upright, 2 clockwise, 3 upside down, 1 counter-clockwise.
    void deviceOrientation(int value);
    // inkview made the task visible (EVT_SHOW); the firmware applied its orientation just before.
    void taskShown();
    // EVT_BACKGROUND: the task went behind the firmware (standby lock screen, another task).
    void taskBackground();
    // USB: EVT_USBSTORE_IN (163) / EVT_USBSTORE_OUT (164), EVT_FSINCOMING (72) / EVT_FSCHANGED (73).
    void usbEvent(int type);

private:
    static int handlerStatic(int type, int par1, int par2);
    int handle(int type, int par1, int par2);
    void pump();
    static int mapKey(int ivKey);

    PbInkScreen *mScreen;
    QTimer mTimer;
    bool mDebug;
    bool mDown = false;
    int mLastOrientation = -1;
    QPoint mLast;
    static PbInkEvents *sInstance;
};

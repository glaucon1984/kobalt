// Pumps inkview's event loop from Qt's and turns inkview events into Qt input events.
#pragma once
#include <QObject>
#include <QTimer>
#include <QPoint>
#include <QVector>
#include <QProcess>
#include <QElapsedTimer>

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
    // Silent connection to a known network in the background, in two steps that never block
    // the GUI thread and never open the firmware's network chooser: the firmware's scan process
    // (WiFiScanProcessStart, polled) to see whether one of the configured networks
    // (EnumWirelessNetworks) is in range, and only then the firmware's agent
    // (/ebrmain/bin/netagent connect) as a child process. Both NetConnectAsync and a bare agent
    // connect open the chooser over the task when nothing known answers, and each cancel
    // reached the host as a background/foreground pair that looked like a wake, which asked
    // again. NetConnectSilent does not open it but blocks the GUI thread for the firmware's 28 s
    // search when the network is absent (seen after a power cut). `done(connected, context)`
    // runs on the GUI thread once: no known network in range, the agent exited, the link is up,
    // or the safety limit passed. Several callers share one attempt.
    typedef void (*NetworkDone)(int connected, void *context);
    void connectNetworkAsync(NetworkDone done, void *context);

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
    void networkDone(int status, const char *how);
    void pollNetwork();
    bool knownNetworkInRange(bool *scanned);
    bool mConnecting = false, mScanning = false;
    QElapsedTimer mScanClock;
    QTimer mNetworkLimit, mNetworkPoll;
    QProcess *mNetAgent = nullptr;
    QVector<QPair<NetworkDone, void *>> mNetworkWaiters;

    PbInkScreen *mScreen;
    QTimer mTimer;
    bool mDebug;
    bool mDown = false;
    int mLastOrientation = -1;
    QPoint mLast;
    static PbInkEvents *sInstance;
};

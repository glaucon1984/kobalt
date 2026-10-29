// Screen for the pbink platform plugin: composes Qt windows into inkview's task framebuffer
// and asks inkview for the e-ink refresh of the touched area. Supports rotating the logical
// screen by 0/90/180/270 degrees on the fixed portrait panel.
#pragma once
#include <QtFbSupport/private/qfbscreen_p.h>
#include <QElapsedTimer>
#include <QTransform>

struct icanvas_s;
class QPainter;

class PbInkScreen : public QFbScreen
{
    Q_OBJECT
public:
    enum Refresh { PartialRefresh, FastRefresh, FullRefresh };

    explicit PbInkScreen(const QStringList &args);
    ~PbInkScreen() override;

    bool initialize() override;
    QRegion doRedraw() override;

    // The next redraw refreshes the whole panel with a full (flashing) update.
    void requestFullRefresh() { mNext = FullRefresh; }
    // Repaint everything (after the task came back to the foreground) with a full refresh.
    void repaintAll();
    void setDefaultRefresh(Refresh r) { mDefault = r; }
    // The next frame is revealed strip by strip (page-turn animation); +1 = forward turn
    // (right to left), -1 = backward (left to right).
    void animateNext(int direction) { mAnimate = direction; }

    // Clockwise rotation of the logical screen on the panel: 0, 90, 180 or 270 degrees.
    void setRotation(int degrees);
    // Show this Qt orientation on the portrait panel and report it to QScreen.
    void setOrientation(Qt::ScreenOrientation orientation);
    // Keep inkview's own task orientation at portrait (checked at start and when shown again).
    void lockPortrait();
    int rotation() const { return mRotation; }
    // Panel (touch) coordinates to logical screen coordinates.
    QPoint toLogical(const QPoint &panel) const { return mToLogical.map(panel); }

private:
    QStringList mArgs;
    icanvas_s *mFb = nullptr;
    QImage mFbImage;
    QPainter *mBlitter = nullptr;
    Refresh mDefault = PartialRefresh;
    Refresh mNext = FullRefresh; // first paint flashes once
    bool mDebug = false;
    int mRotation = 0;
    int mAnimate = 0;
    int mPanelWidth = 0, mPanelHeight = 0, mDpi = 300;
    QTransform mToPanel, mToLogical;
    // Time of the last update issued; a frame that follows another closely waits for the
    // panel first and goes without the strip wipe.
    QElapsedTimer mLastUpdate;
    int mQuickTurns = 0; // consecutive frames within 0.7 s of the previous update
};

#include "pbinkscreen.h"
#include <QPainter>
#include <QScreen>
#include <qpa/qwindowsysteminterface.h>
#include <QtCore/QLoggingCategory>
#include <QtMath>
#include <QElapsedTimer>
#include <inkview.h> // has its own extern "C" guards
#include <arm_neon.h>
#include <stdlib.h>
#include <unistd.h>

Q_LOGGING_CATEGORY(lcPbInk, "qt.qpa.pbink")

namespace {
// inkview's update calls normally return within a few ms; a call that blocks for long shows up
// in the log with its name (suspected cause of a book stuck on its loading screen).
template <typename Call>
void timed(const char *name, Call call)
{
    QElapsedTimer clock;
    clock.start();
    call();
    const qint64 ms = clock.elapsed();
    if (ms > 300)
        qWarning("pbink: inkview %s took %lld ms", name, static_cast<long long>(ms));
}

// Eight pixels of RGB32 to 8-bit grey with qGray()'s weights. The whole build targets NEON
// (pocketbook-gcc12.toolchain.cmake).
inline uint8x8_t greyPixels(const QRgb *source)
{
    const uint8x8x4_t bgra = vld4_u8(reinterpret_cast<const uint8_t *>(source));
    uint16x8_t sum = vmull_u8(bgra.val[2], vdup_n_u8(11));
    sum = vmlal_u8(sum, bgra.val[1], vdup_n_u8(16));
    sum = vmlal_u8(sum, bgra.val[0], vdup_n_u8(5));
    return vshrn_n_u16(sum, 5);
}

void greyRow(const QRgb *source, uchar *target, int count)
{
    int x = 0;
    for (; x + 8 <= count; x += 8)
        vst1_u8(target + x, greyPixels(source + x));
    for (; x < count; ++x)
        target[x] = uchar(qGray(source[x]));
}

// The same for a screen turned upside down: target is where source[0] goes, the row runs backwards.
void greyRowBackwards(const QRgb *source, uchar *target, int count)
{
    int x = 0;
    for (; x + 8 <= count; x += 8)
        vst1_u8(target - x - 7, vrev64_u8(greyPixels(source + x)));
    for (; x < count; ++x)
        target[-x] = uchar(qGray(source[x]));
}
}

PbInkScreen::PbInkScreen(const QStringList &args)
    : mArgs(args)
{
    mDebug = args.contains(QLatin1String("debug"));
}

PbInkScreen::~PbInkScreen()
{
    delete mBlitter;
}

bool PbInkScreen::initialize()
{
    // OpenScreen() is what InkViewMain() does before entering its loop: it attaches the task
    // framebuffer, sets up the touch panel and the orientation. We keep Qt's loop instead.
    // Tell inkview before OpenScreen() that this app handles orientation events itself.
    setenv("SUPPORT_EVT_ORIENTATION", "YES", 1);
    OpenScreen();
    // The previous task may have left the panel busy (quick reopen): wait for it before painting.
    timed("WaitForUpdateComplete", [] { WaitForUpdateComplete(); });
    // The firmware starts the task with the orientation the device is held in (its own
    // auto-rotation) and keeps it for the task's lifetime. All rotation is done here instead,
    // so pin the task framebuffer to the physical portrait panel.
    lockPortrait();
    mFb = GetTaskFramebuffer(GetCurrentTask());
    if (!mFb || !mFb->addr) {
        qWarning("pbink: GetTaskFramebuffer() returned nothing");
        return false;
    }
    QImage::Format fbFormat;
    switch (mFb->depth) {
    case 8: fbFormat = QImage::Format_Grayscale8; break;
    case 16: fbFormat = QImage::Format_RGB16; break;
    case 24: fbFormat = QImage::Format_RGB888; break;
    case 32: fbFormat = QImage::Format_RGB32; break;
    default:
        qWarning("pbink: unsupported framebuffer depth %d", mFb->depth);
        return false;
    }
    mDpi = get_screen_dpi();
    if (mDpi <= 0)
        mDpi = 300;
    mPanelWidth = mFb->width;
    mPanelHeight = mFb->height;
    mGeometry = QRect(0, 0, mPanelWidth, mPanelHeight);
    // Compose in 32-bit colour; the blit to the panel converts to the panel's format.
    mDepth = 32;
    mFormat = QImage::Format_RGB32;
    mPhysicalSize = QSizeF(mPanelWidth * 25.4 / mDpi, mPanelHeight * 25.4 / mDpi);
    QFbScreen::initializeCompositor();
    mFbImage = QImage(mFb->addr, mPanelWidth, mPanelHeight, mFb->scanline, fbFormat);
    qInfo("pbink: screen %dx%d, panel depth %d, scanline %d, %d dpi, model %s, firmware %s, sleepmode %d",
          mPanelWidth, mPanelHeight, mFb->depth, mFb->scanline, mDpi, GetDeviceModel(), GetSoftwareVersion(),
          GetSleepmode());
    return true;
}

// inkview orientation codes: 0 portrait, 1 landscape 90, 2 landscape 270, 3 portrait 180.
void PbInkScreen::lockPortrait()
{
    const int current = GetOrientation();
    if (current != 0) {
        qInfo("pbink: task orientation was %d, forcing portrait", current);
        SetOrientation(0);
    }
}

void PbInkScreen::setRotation(int degrees)
{
    degrees = ((degrees % 360) + 360) % 360;
    if (degrees % 90 != 0 || degrees == mRotation || !mFb)
        return;
    mRotation = degrees;
    const int pw = mPanelWidth, ph = mPanelHeight;
    // Logical (x, y) -> panel: content turned clockwise by mRotation on the fixed panel.
    mToPanel = QTransform();
    switch (mRotation) {
    case 90: mToPanel.translate(pw, 0); mToPanel.rotate(90); break;
    case 180: mToPanel.translate(pw, ph); mToPanel.rotate(180); break;
    case 270: mToPanel.translate(0, ph); mToPanel.rotate(270); break;
    default: break;
    }
    mToLogical = mToPanel.inverted();
    const bool sideways = mRotation == 90 || mRotation == 270;
    const QSize logical = sideways ? QSize(ph, pw) : QSize(pw, ph);
    mPhysicalSize = QSizeF(logical.width() * 25.4 / mDpi, logical.height() * 25.4 / mDpi);
    delete mBlitter;
    mBlitter = nullptr;
    // Reallocates the compositor image, tells Qt and resizes full-screen windows.
    QFbScreen::setGeometry(QRect(QPoint(0, 0), logical));
    qInfo("pbink: rotation %d, logical screen %dx%d", mRotation, logical.width(), logical.height());
    repaintAll();
}

void PbInkScreen::setOrientation(Qt::ScreenOrientation orientation)
{
    // Qt's own convention for a portrait panel: QScreen::angleBetween(Portrait, orientation).
    int degrees = 0;
    switch (orientation) {
    case Qt::LandscapeOrientation: degrees = 270; break;
    case Qt::InvertedPortraitOrientation: degrees = 180; break;
    case Qt::InvertedLandscapeOrientation: degrees = 90; break;
    default: orientation = Qt::PortraitOrientation; break;
    }
    setRotation(degrees);
    if (QScreen *s = screen()) {
        s->setOrientationUpdateMask(Qt::PortraitOrientation | Qt::LandscapeOrientation |
                                    Qt::InvertedPortraitOrientation | Qt::InvertedLandscapeOrientation);
        QWindowSystemInterface::handleScreenOrientationChange(s, orientation);
    }
}

QRegion PbInkScreen::doRedraw()
{
    QElapsedTimer composing;
    composing.start();
    const QRegion touched = QFbScreen::doRedraw();
    if (touched.isEmpty())
        return touched;
    const qint64 composeMs = composing.elapsed();
    if (!mBlitter) {
        mBlitter = new QPainter(&mFbImage);
        mBlitter->setCompositionMode(QPainter::CompositionMode_Source);
        mBlitter->setTransform(mToPanel);
    }
    const Refresh kind = mNext;
    mNext = mDefault;
    const int sweep = mAnimate;
    mAnimate = 0;
    const bool wipe = sweep && kind != FullRefresh;
    // An update that lands on one the panel is still drawing tears, and GC16 takes 388 ms here,
    // so no spot may be redrawn within about 430 ms. WaitForUpdateComplete() returns at once on
    // this firmware, so the pacing is by time, from the last update issued. A wipe in the same
    // direction as the wipe before it reaches each spot PBINK_MIN_FRAME_MS (300) plus the length
    // of a wipe later: keep the sum above 388 ms when lowering the gap or shortening the wipe.
    // Every other large frame reaches spots drawn moments ago and waits PBINK_LONG_GAP_MS (475):
    // a wipe the other way starts where the last one ended, and a plain or full frame redraws
    // everything at once. Both follow from swipeanimation.koplugin's 250 and 430 ms: it sleeps,
    // then spends about 46 ms drawing before the first strip, while this frame is already composed.
    static const int minFrameMs = qEnvironmentVariableIsSet("PBINK_MIN_FRAME_MS")
                                      ? qEnvironmentVariableIntValue("PBINK_MIN_FRAME_MS") : 300;
    static const int longGapMs = qEnvironmentVariableIsSet("PBINK_LONG_GAP_MS")
                                     ? qEnvironmentVariableIntValue("PBINK_LONG_GAP_MS") : 475;
    const qint64 sinceLast = mLastUpdate.isValid() ? mLastUpdate.elapsed() : 1000000;
    // Only a frame that covers a good part of the screen (a page, a full-width panel) is paced.
    // A small region (the light pop-up while a finger slides along the edge, a button) is a
    // quick update the controller absorbs; pacing it made this thread sleep up to the gap per
    // frame while the touch events kept arriving, so the pop-up trailed further behind the
    // finger the longer the drag went on. A full refresh redraws the panel whatever was touched.
    const QRect bounds = touched.boundingRect();
    const bool large = kind == FullRefresh || qint64(bounds.width()) * bounds.height() * 4 >=
                                                  qint64(mScreenImage.width()) * mScreenImage.height();
    if (large) {
        const int gapMs = wipe && sweep == mLastSweep ? minFrameMs : longGapMs;
        if (sinceLast < gapMs) {
            usleep(useconds_t(gapMs - sinceLast) * 1000);
            if (mDebug)
                qCDebug(lcPbInk, "paced the next frame by %lld ms", static_cast<long long>(gapMs - sinceLast));
        }
        mLastSweep = wipe ? sweep : 0;
    }
    if (wipe) {
        // Strip wipe as in swipeanimation.koplugin's e-ink engine: the whole new frame goes into
        // the framebuffer first (unrefreshed pixels keep showing the old page), then vertical
        // strips are refreshed one after another, a short pause apart. Forward turns reveal from
        // the right edge to the left, backward turns the other way. PBINK_ANIM_BANDS (default 8),
        // PBINK_ANIM_MS (default 16). DynamicUpdate asks for GC16 as PartialUpdate does on
        // firmware 6.11, it is not A2, yet looks cleaner on the panel: PBINK_ANIM_FAST=0 to compare.
        static const int bands = qBound(2, qEnvironmentVariableIntValue("PBINK_ANIM_BANDS") > 0
                                               ? qEnvironmentVariableIntValue("PBINK_ANIM_BANDS") : 8, 32);
        static const int pauseMs = qEnvironmentVariableIsSet("PBINK_ANIM_MS")
                                       ? qEnvironmentVariableIntValue("PBINK_ANIM_MS") : 16;
        static const bool fast = !qEnvironmentVariableIsSet("PBINK_ANIM_FAST")
                                 || qEnvironmentVariableIntValue("PBINK_ANIM_FAST") == 1;
        QElapsedTimer copying;
        copying.start();
        blit(touched);
        const qint64 copyMs = copying.elapsed();
        const QRect area = touched.boundingRect();
        QElapsedTimer striping;
        striping.start();
        for (int i = 0; i < bands; ++i) {
            const int index = sweep > 0 ? bands - 1 - i : i;
            const int left = area.x() + area.width() * index / bands;
            const int right = area.x() + area.width() * (index + 1) / bands;
            const QRect strip = QRect(left, area.y(), right - left, area.height()).intersected(area);
            if (strip.isEmpty())
                continue;
            const QRect panel = mToPanel.mapRect(strip).intersected(QRect(0, 0, mPanelWidth, mPanelHeight));
            if (fast)
                timed("DynamicUpdate", [&] { DynamicUpdate(panel.x(), panel.y(), panel.width(), panel.height()); });
            else
                timed("PartialUpdate", [&] { PartialUpdate(panel.x(), panel.y(), panel.width(), panel.height()); });
            if (i + 1 < bands && pauseMs > 0)
                usleep(useconds_t(pauseMs) * 1000);
        }
        mLastUpdate.start();
        // Logged after the strips, not to delay them: the first strip left striping ms before
        // this line, which is what to compare with the tap's event line.
        if (mDebug)
            qCDebug(lcPbInk, "wipe %d: composed in %lld ms, copied in %lld ms, strips took %lld ms", sweep,
                    static_cast<long long>(composeMs), static_cast<long long>(copyMs),
                    static_cast<long long>(striping.elapsed()));
        return touched;
    }
    blit(touched);

    const QRect r = mToPanel.mapRect(touched.boundingRect()).intersected(QRect(0, 0, mPanelWidth, mPanelHeight));
    if (mDebug)
        qCDebug(lcPbInk) << "refresh" << kind << r;
    switch (kind) {
    case FullRefresh:
        timed("FullUpdate", [] { FullUpdate(); });
        break;
    case FastRefresh:
        timed("DynamicUpdate", [&] { DynamicUpdate(r.x(), r.y(), r.width(), r.height()); });
        break;
    case PartialRefresh:
        timed("PartialUpdate", [&] { PartialUpdate(r.x(), r.y(), r.width(), r.height()); });
        break;
    }
    mLastUpdate.start();
    return touched;
}

// The composed screen into the panel's framebuffer. QPainter's generic path took 22 ms for a page
// on the Era Lite upright, 38 upside down and 189 sideways. A sideways frame steps a whole panel
// line per pixel, so it goes in tiles that fit the 32 KB data cache.
void PbInkScreen::blit(const QRegion &region)
{
    if (mFbImage.format() != QImage::Format_Grayscale8) {
        for (const QRect &rect : region)
            mBlitter->drawImage(rect, mScreenImage, rect);
        return;
    }
    // Where logical pixel (0, 0) lands on the panel, and how far one pixel right or down moves it.
    const QPointF start = mToPanel.map(QPointF(0.5, 0.5));
    const QPointF right = mToPanel.map(QPointF(1.5, 0.5)) - start;
    const QPointF down = mToPanel.map(QPointF(0.5, 1.5)) - start;
    const int line = mFbImage.bytesPerLine();
    const int stepX = qRound(right.x()) + qRound(right.y()) * line;
    const int stepY = qRound(down.x()) + qRound(down.y()) * line;
    uchar *origin = mFbImage.bits() + qFloor(start.y()) * line + qFloor(start.x());
    static const int tile = 64;
    for (const QRect &rect : region) {
        const QRect area = rect.intersected(mScreenImage.rect());
        const int bottom = area.y() + area.height(), end = area.x() + area.width();
        if (stepX == 1 || stepX == -1) {
            for (int y = area.y(); y < bottom; ++y) {
                const QRgb *source = reinterpret_cast<const QRgb *>(mScreenImage.constScanLine(y)) + area.x();
                uchar *target = origin + y * stepY + area.x() * stepX;
                if (stepX == 1)
                    greyRow(source, target, area.width());
                else
                    greyRowBackwards(source, target, area.width());
            }
            continue;
        }
        for (int top = area.y(); top < bottom; top += tile) {
            const int tileBottom = qMin(top + tile, bottom);
            for (int left = area.x(); left < end; left += tile) {
                const int count = qMin(tile, end - left);
                for (int y = top; y < tileBottom; ++y) {
                    const QRgb *source = reinterpret_cast<const QRgb *>(mScreenImage.constScanLine(y)) + left;
                    uchar *target = origin + y * stepY + left * stepX;
                    for (int x = 0; x < count; ++x, target += stepX)
                        *target = uchar(qGray(source[x]));
                }
            }
        }
    }
}

void PbInkScreen::repaintAll()
{
    mNext = FullRefresh;
    setDirty(mGeometry);
}

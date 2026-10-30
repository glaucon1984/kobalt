#include "pbinkscreen.h"
#include <QPainter>
#include <QScreen>
#include <qpa/qwindowsysteminterface.h>
#include <QtCore/QLoggingCategory>
#include <QElapsedTimer>
#include <inkview.h> // has its own extern "C" guards
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
    const QRegion touched = QFbScreen::doRedraw();
    if (touched.isEmpty())
        return touched;
    if (!mBlitter) {
        mBlitter = new QPainter(&mFbImage);
        mBlitter->setCompositionMode(QPainter::CompositionMode_Source);
        mBlitter->setTransform(mToPanel);
    }
    const Refresh kind = mNext;
    mNext = mDefault;
    const int sweep = mAnimate;
    mAnimate = 0;
    // The controller executes partial updates asynchronously, a strip wipe queues ten of them,
    // and a page turned before they are done used to be blitted under them: strips of two pages
    // on the panel. WaitForUpdateComplete() returns at once on this firmware, so the pacing is
    // by time: a frame follows the previous update by at least PBINK_MIN_FRAME_MS (350 ms, a
    // little under one partial update), and from the PBINK_ANIM_BURST-th frame within 700 ms of
    // the previous one onwards the wipe is dropped.
    static const int minFrameMs = qEnvironmentVariableIsSet("PBINK_MIN_FRAME_MS")
                                      ? qEnvironmentVariableIntValue("PBINK_MIN_FRAME_MS") : 350;
    // The controller absorbs a few wipes in a row before its queue falls behind, so a burst keeps
    // the wipe for its first PBINK_ANIM_BURST turns (default 4) and drops it from then on until
    // the turns slow down. First measured at 450 ms pacing: three quick wipes were fine and the
    // fourth tore; the defaults were then tightened to 350 ms and four turns (2026-10-29).
    static const int burstWipes = qEnvironmentVariableIsSet("PBINK_ANIM_BURST")
                                      ? qEnvironmentVariableIntValue("PBINK_ANIM_BURST") : 4;
    const qint64 sinceLast = mLastUpdate.isValid() ? mLastUpdate.elapsed() : 1000000;
    // Only a frame that covers a good part of the screen (a page, a full-width panel) is paced
    // and counted as a turn. A small region (the light pop-up while a finger slides along the
    // edge, a button) is a quick update the controller absorbs; pacing it made this thread sleep
    // up to minFrameMs per frame while the touch events kept arriving, so the pop-up trailed
    // further behind the finger the longer the drag went on.
    const QRect bounds = touched.boundingRect();
    const bool large = qint64(bounds.width()) * bounds.height() * 4 >=
                       qint64(mScreenImage.width()) * mScreenImage.height();
    bool quickSuccession = false;
    if (large) {
        if (sinceLast < 700)
            ++mQuickTurns;
        else
            mQuickTurns = 0;
        quickSuccession = mQuickTurns >= burstWipes;
        if (sinceLast < minFrameMs) {
            usleep(useconds_t(minFrameMs - sinceLast) * 1000);
            if (mDebug)
                qCDebug(lcPbInk, "paced the next frame by %lld ms", static_cast<long long>(minFrameMs - sinceLast));
        }
    }
    if (sweep && kind != FullRefresh && !quickSuccession) {
        // Strip wipe as in swipeanimation.koplugin's e-ink engine: the whole new frame goes into
        // the framebuffer first (unrefreshed pixels keep showing the old page), then vertical
        // strips are refreshed one after another with the ordinary partial waveform, a short
        // pause apart. Forward turns reveal from the right edge to the left, backward turns the
        // other way. PBINK_ANIM_BANDS (default 10), PBINK_ANIM_MS (default 20), PBINK_ANIM_FAST=1
        // for the A2 waveform instead.
        static const int bands = qBound(2, qEnvironmentVariableIntValue("PBINK_ANIM_BANDS") > 0
                                               ? qEnvironmentVariableIntValue("PBINK_ANIM_BANDS") : 10, 32);
        static const int pauseMs = qEnvironmentVariableIsSet("PBINK_ANIM_MS")
                                       ? qEnvironmentVariableIntValue("PBINK_ANIM_MS") : 20;
        static const bool fast = qEnvironmentVariableIntValue("PBINK_ANIM_FAST") == 1;
        for (const QRect &rect : touched)
            mBlitter->drawImage(rect, mScreenImage, rect);
        const QRect area = touched.boundingRect();
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
        return touched;
    }
    for (const QRect &rect : touched)
        mBlitter->drawImage(rect, mScreenImage, rect);

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

void PbInkScreen::repaintAll()
{
    mNext = FullRefresh;
    setDirty(mGeometry);
}

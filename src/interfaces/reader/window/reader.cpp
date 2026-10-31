#include "interfaces/reader/ui/controls/buttons.h"
#include "interfaces/reader/ui/controls/icons.h"
#include "interfaces/reader/ui/controls/paging.h"
#include "interfaces/reader/ui/controls/panels.h"
#include "interfaces/reader/ui/dialogs/modals.h"
#include "interfaces/reader/ui/style/theme.h"
#include "reader_p.h"
#include "support/qt_compat.h"
#include "support/strings.h"
#include "ui/input.h"
#include "interfaces/reader/ui/style/fonts.h"
#include "interfaces/reader/views/sleep_screen.h"
#include "interfaces/reader/views/kosync_notice.h"
#include "interfaces/reader/ui/style/styles.h"
#include "platform/api/kobo_refresh.h"
#include "reading/appearance_settings.h"
#include "reading/page_navigation.h"
#include "reading/sleep_screen.h"
#include "model/page_refresh_regions.h"
#include "support/diagnostics.h"
#include "interfaces/reader/ui/style/metrics.h"
#include "interfaces/reader/ui/panel_timing.h"
#include "storage/profile_database.h"
#include "storage/reader_storage.h"
#include <QApplication>
#include <QJsonArray>
#include <QFontInfo>
#include <QHideEvent>
#include <QResizeEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QScreen>
#include <QWindow>
#include <QShowEvent>
#include <QTime>
#include <QTouchEvent>
#include <utility>

namespace kobalt::reader {
namespace strings = kobalt::strings;
using namespace kobalt::storage;
namespace m = kobalt::ui::metrics;
using kobalt::ui::makeButton;
using kobalt::platform::textRefresh;
using kobalt::Choice;

Reader::~Reader() {
    // Member destruction can send events before QObject removes our filters.
    qApp->removeEventFilter(this);
    for (auto *widget : findChildren<QWidget *>())
        widget->removeEventFilter(this);
    // Qt drops these connections only in ~QObject, after the members, the book
    // among them, are gone. Hiding the window in ~QWidget can change the
    // application state and would call back into a half-destroyed reader.
    disconnect(qApp, nullptr, this, nullptr);
    disconnect(QGuiApplication::primaryScreen(), nullptr, this, nullptr);
    delete annotationExportPicker.data();
    cancelKosync();
    kosyncPage->dismissEditor();
    book->onEstimatesChanged({});
    model->cancelPagePreview();
    if (pageScrubber) {
        pageScrubber->cancel();
        pageScrubber->cancelled = {};
    }
    model->renderProgressChanged = {};
    renderNoticeTimer.stop();
    model->cancelPendingTurn();
    model->turnBoundary = {};
    page->cancelHold();
    brightnessHide.stop();
    annotationModeHide.stop();
    lightPopup->changed = {};
    delete lightPopup;
    light.reset();
    presentation->clearSleepScreen();
    presentation->resumeBackground();
    kobalt::ui::setInterfaceFontFamily({});
    kobalt::ui::setInterfaceMonoFontFamily({});
    kobalt::ui::setHeaderFooterFontFamily({});
    kobalt::ui::setHeaderFooterAddedWeight(2);
}

void Reader::checkpoint() {
    // Hosts can checkpoint before our application-state callback. Freeze the
    // clock before any save can block, so background I/O is not reading time.
    if (services.suspended())
        book->stopReading();
    save(true);
}

void Reader::observeReading() {
    if (closing || lifecycleHidden || !isVisible() || services.suspended()) {
        book->stopReading();
        return;
    }
    const bool wasReading = book->readingActive();
    book->observeReading(!topControls->isVisible() && !general->isVisible() && !choices->isVisible() &&
                         !dictionaryPanel->isVisible() && !searchPanel->isVisible() && !linksActive() &&
                         !appearanceEdit.panelOpen() && !syncDialog && !kosyncDialog && !finishDialog);
    if (clockTimer && book->readingActive() != wasReading)
        startClock();
}

void Reader::prepareShutdown(std::function<void()> finished) {
    prepareTransition(std::move(finished), false);
}
void Reader::prepareUsb(std::function<void()> finished) {
    prepareTransition(std::move(finished), true);
}
void Reader::prepareTransition(std::function<void()> finished, bool usb) {
    constexpr int timeoutMs = 2000;
    const auto timeoutEvent = usb ? "usb-save-timeout" : "shutdown-save-timeout";
    if (pageScrubber)
        pageScrubber->cancel();
    auto backdrop = usb ? powerScreenSnapshot() : sleep::Snapshot{};
    sleepImage = {};
    presentation->clearSleepScreen();
    auto elapsed = std::make_shared<QElapsedTimer>();
    elapsed->start();
    model->cancelPendingTurn();
    book->stopReading();
    lifecycleHidden = true;
    exitPending = false;
    closing = true;
    cancelKosync();
    kosyncPage->dismissEditor();
    clockTimer->stop();
    setEnabled(false);
    // USB export and power-off need running annotation saves on storage.
    bookmarks->flush();
    highlights->flush();
    save(true);
    auto completed = std::make_shared<bool>(false);
    kobalt::qt::later(qMax(0, timeoutMs - int(elapsed->elapsed())), this, [completed, finished, timeoutEvent] {
        if (std::exchange(*completed, true)) {
            return;
        }
        kobalt::diagnostic(timeoutEvent);
        finished();
    });
    model->nativePosition(
        [this, completed, elapsed, usb, timeoutEvent, finished, backdrop = std::move(backdrop)](const QString &location, const QString &error) {
            if (std::exchange(*completed, true)) {
                return;
            }
            if (elapsed->elapsed() >= timeoutMs) {
                kobalt::diagnostic(timeoutEvent);
            } else {
                // Keep a newer Nickel bookmark if reconciliation or the locked
                // comparison rejects this save. Device transitions cannot ask
                // the reader to resolve a conflict before they continue.
                book->writeNative(location, error);
                save(true);
                if (usb)
                    presentation->saveSleepScreen(backdrop);
            }
            finished();
        });
}

void Reader::prepareSleepCover() {
    const auto request = ++sleepCoverRequest;
    sleepCover = {};
    if (!presentation->supportsSleepScreen() || sleep::mode(preferences) != sleep::Mode::Cover) {
        diagnostic("sleep-cover-skipped", {{"supported", presentation->supportsSleepScreen()},
                                           {"mode", sleep::value(sleep::mode(preferences))}});
        return;
    }
    // Decode off the GUI thread while the book is available. Keep at most
    // 16 MiB, only for the cover option, and reuse it in either orientation.
    QPointer<Reader> guard(this);
    model->coverImage(
        {2048, 2048},
        [guard, request](QImage image) {
            diagnostic("sleep-cover", {{"ok", !image.isNull()},
                                       {"width", image.width()},
                                       {"height", image.height()},
                                       {"current", guard && guard->sleepCoverRequest == request}});
            if (guard && guard->sleepCoverRequest == request) {
                guard->sleepCover = std::move(image);
            }
        },
        services.coverImagePath);
}

sleep::Snapshot Reader::powerScreenSnapshot() {
    if (exportRunning)
        return exitSleepSnapshot;
    if (lifecycleHidden && !sleepImage.isNull())
        return {sleepImage, sleepOrientation, sleepProgress, sleepRemaining, darkTheme, sleep::mode(preferences), false,
                sleepPercent, sleep::tile(preferences)};
    return captureSleepScreen();
}

sleep::Snapshot Reader::captureSleepScreen() {
    // RGB32 needs four bytes per pixel. Bound the optional capture to 16 MiB;
    // oversized viewports keep Nickel's power screen instead.
    const qint64 pixels = qint64(page->width()) * page->height();
    const auto *modal = QApplication::activeModalWidget();
    const bool foreground = !modal || modal == this || isAncestorOf(modal);
    const auto mode = sleep::mode(preferences);
    if (mode == sleep::Mode::Stock || !presentation->supportsSleepScreen() || !foreground || pixels <= 0 ||
        pixels > 4 * 1024 * 1024 || !readyForPresentation() || appearanceEdit.panelOpen() || model->temporaryTypography())
        return {};
    const bool useCover = mode == sleep::Mode::Cover && !sleepCover.isNull();
    auto frame = useCover ? cachedSleepCoverBackground(page->size()) : page->snapshot(true);
    if (frame.isNull())
        return {};
    const int percent = pageProgressPercent(model->page(), model->pages());
    return {std::move(frame), kobalt::shownOrientation(QGuiApplication::primaryScreen()),
            strings::get("sleep.percent_read").arg(percent), sleepTimeLeft(book->estimates().bookMilliseconds),
            darkTheme, mode, !useCover, percent, sleep::tile(preferences)};
}

// The sleep image for the current page and battery state, composed the way sleepReader() does
// it, without entering the sleep sequence. Null while the page cannot serve as a sleep screen
// (stock sleep screen chosen, a panel open, the engine still laying out).
// The cover fitted to the page, kept between compositions: a host that keeps a cached sleep
// screen current composes after every page turn, and the scaling was most of the cost.
QImage Reader::cachedSleepCoverBackground(QSize size) {
    static qint64 coverKey = -1;
    static QSize coverSize;
    static QImage background;
    if (background.isNull() || coverKey != sleepCover.cacheKey() || coverSize != size) {
        background = sleepCoverBackground(sleepCover, size);
        coverKey = sleepCover.cacheKey();
        coverSize = size;
    }
    return background; // the composition paints on its own copy (implicit sharing)
}

QImage Reader::composeSleepImage() {
    auto snapshot = captureSleepScreen();
    if (snapshot.image.isNull())
        return {};
    const auto battery = services.battery();
    const auto status = battery.fullyCharged ? SleepStatus::Charged
                        : battery.charging   ? SleepStatus::Charging
                                             : SleepStatus::Sleeping;
    return sleepScreenImage(std::move(snapshot.image), snapshot.progress, snapshot.remaining, darkTheme,
                            battery.percent, status, snapshot.dimPage, snapshot.tile, snapshot.percent);
}

void Reader::sleepReader() {
    if (orientation) {
        orientation->freeze();
    }
    if (pageScrubber)
        pageScrubber->cancel();
    if (lifecycleHidden)
        return;
    setProperty("reader_sleep_clean_pending", true);
    model->cancelPendingTurn();
    book->stopReading();
    auto snapshot = captureSleepScreen();
    sleepOrientation = snapshot.orientation;
    sleepProgress = snapshot.progress;
    sleepRemaining = snapshot.remaining;
    sleepPercent = snapshot.percent;
    sleepBattery = services.battery();
    const auto status = sleepBattery.fullyCharged ? SleepStatus::Charged
                        : sleepBattery.charging ? SleepStatus::Charging : SleepStatus::Sleeping;
    sleepImage = sleepScreenImage(std::move(snapshot.image), sleepProgress, sleepRemaining, darkTheme,
                                  sleepBattery.percent, status, snapshot.dimPage, snapshot.tile, sleepPercent);
    services.cancelAnimation();
    page->animationDirection = 0;
    setProperty("reader_animation_direction", 0);
    appearanceEdit.clearContinuation();
    saveNativeProgress();
    checkpoint();
    // Fold waiting saves into the main profile file before a long sleep.
    bookmarks->flush();
    highlights->flush();
    kobalt::storage::checkpointProfile();
    closeSearch();
    dismissLookup();
    lifecycleHidden = true;
    hideKosyncOpening();
    kosyncPage->dismissEditor();
    kosyncPage->setStatus(kosyncMessage, false);
    kosyncStarted = false;
    if (kosyncDialog) {
        kosyncDialog->hide();
        kosyncDialog->deleteLater();
        kosyncDialog = nullptr;
    }
    clockTimer->stop();
    refreshRenderNotice();
    if (syncDialog)
        syncDialog->hide();
    if (finishDialog)
        finishDialog->hide();
    flushKosync([] {});
    // Keep the page on screen until Nickel's sleep view replaces it, as in
    // Nickel's own reader. sleepViewReady() submits that view.
}

void Reader::prepareWake() {
    if (lifecycleHidden && !isVisible())
        presentation->pauseBackground(true);
}

void Reader::updateSleepBattery() {
    if (!lifecycleHidden || sleepImage.isNull()) {
        return;
    }
    const auto battery = services.battery();
    if (battery.charging == sleepBattery.charging && battery.fullyCharged == sleepBattery.fullyCharged &&
        ((!battery.charging && !battery.fullyCharged) || battery.percent == sleepBattery.percent)) {
        return;
    }
    sleepBattery = battery;
    const auto status = battery.fullyCharged ? SleepStatus::Charged
                        : battery.charging   ? SleepStatus::Charging
                                             : SleepStatus::Sleeping;
    // Paint over the card in the retained frame. Reusing the composed background
    // avoids another cover decode or another dimming pass while asleep.
    const auto tile = sleep::tile(preferences);
    sleepImage = sleepScreenImage(std::move(sleepImage), sleepProgress, sleepRemaining, darkTheme, battery.percent,
                                  status, false, tile, sleepPercent);
    if (!isVisible()) {
        presentation->updateSleepScreen(sleepImage,
                                        sleepScreenCardRect(sleepImage.size(), !sleepRemaining.isEmpty(), tile));
    }
}

void Reader::cancelWake() {
    if (lifecycleHidden && !isVisible())
        presentation->resumeBackground();
}

void Reader::sleepViewReady() {
    if (!lifecycleHidden || !isVisible())
        return;
    if (!sleepImage.isNull()) {
        presentation->presentSleepScreen(sleepImage, sleepOrientation);
    }
    // Hiding a top-level window immediately submits the backing store
    // underneath it. Paint Nickel's sleep view first, or that store still
    // contains the library. Do not pump unrelated events during handoff.
    // Submit that paint as one full GC16 refresh. It replaces the page, which
    // Nickel's own partial sleep refreshes on colour screens would leave
    // behind, so the panel flashes once on every device.
    setProperty("reader_preparing_handoff", true);
    presentation->resumeAndRepaintBackground();
    setProperty("reader_preparing_handoff", false);
    setProperty("reader_sleep_handoff", true);
    hide();
    setProperty("reader_sleep_handoff", false);
}

void Reader::wakeReader() {
    cancelKosync();
    kosyncStarted = false;
    sleepImage = {};
    presentation->clearSleepScreen();
    setProperty("reader_sleep_clean_pending", false);
    lifecycleHidden = false;
    QTimer::singleShot(0, this, [this] { synchronizeKosync(); });
    if (finishDialog)
        finishDialog->show();
    setProperty("reader_preparing_wake", true);
    if (orientation) {
        orientation->start(kobalt::shownOrientation(QGuiApplication::primaryScreen()));
    }
    showFullScreen();
    book->invalidateProgress();
    startClock();
    refresh();
    scheduleNativeOpeningSave();
    QTimer::singleShot(0, this, [this] {
        for (auto *widget : findChildren<QWidget *>())
            QCoreApplication::sendPostedEvents(widget, QEvent::UpdateLater);
        QCoreApplication::sendPostedEvents(this, QEvent::UpdateLater);
        QCoreApplication::sendPostedEvents(this, QEvent::UpdateRequest);
        setProperty("reader_preparing_wake", false);
        if (!lifecycleHidden && isVisible())
            repaint();
    });
}

void Reader::saveNativeProgress() {
    if (!book->canWriteNative() || model->image().isNull() || !model->error().isEmpty() || appearanceEdit.panelOpen() ||
        importingProgress || syncDialog) {
        return;
    }
    const auto anchor = model->state().value("position").toString();
    if (anchor.isEmpty() ||
        (anchor == savedNativeAnchor && model->page() == savedNativePage && model->pages() == savedNativePages)) {
        return;
    }
    QString error;
    const auto location = model->savedNativePosition(&error);
    if (!book->writeNative(location, error)) {
        return;
    }
    QJsonObject state;
    book->checkpoint(&state);
    const auto result = state.value("native_writeback").toString();
    if (result == "verified" || result == "time-only") {
        savedNativeAnchor = anchor;
        savedNativePage = model->page();
        savedNativePages = model->pages();
        if (nativeOpeningSaveTimer)
            nativeOpeningSaveTimer->stop();
    }
}

void Reader::scheduleNativeOpeningSave() {
    if (!book->hasNativeProgress() || !savedNativeAnchor.isEmpty() || closing || exportRunning || lifecycleHidden ||
        !isVisible())
        return;
    if (!nativeOpeningSaveTimer) {
        nativeOpeningSaveTimer = new QTimer(this);
        nativeOpeningSaveTimer->setObjectName("rrNativeOpeningSaveTimer");
        nativeOpeningSaveTimer->setSingleShot(true);
        connect(nativeOpeningSaveTimer, &QTimer::timeout, this, [this] {
            // An opening is presented before Nickel necessarily accepts its
            // progress. Retry only this unfinished write, after input settles.
            if (!closing && !exportRunning && !lifecycleHidden && isVisible() && !services.suspended() &&
                !model->busy() && !book->deviceTransition() &&
                (!lastInput.isValid() || lastInput.elapsed() >= 2000))
                saveNativeProgress();
            scheduleNativeOpeningSave();
        });
    }
    if (!nativeOpeningSaveTimer->isActive())
        nativeOpeningSaveTimer->start(2000);
}

void Reader::progressTick() {
    if (closing || exportRunning)
        return;
    if (services.suspended() || book->deviceTransition() || !isVisible()) {
        if (syncDialog)
            syncDialog->hide();
        return;
    }
    if (importingProgress)
        return;
    if (progressBlocked()) {
        if (syncDialog)
            syncDialog->hide();
        return;
    }
    book->pollProgress();
    showProgressChoice();
}

QVector<QWidget *> Reader::popups() const {
    // Header and footer and the field list cover the panels, under every other pop-up.
    return {annotationPopover,
            headerFooterView,
            fieldChooser,
            busyNotice,
            brightnessNotice,
            annotationModeNotice,
            fontDefaultDialog.data(),
            settingsFontPopup.data(),
            kosyncPage ? kosyncPage->editorDialog.data() : nullptr,
            kosyncDialog.data(),
            linkDialog.data(),
            debugDialog.data(),
            syncDialog.data(),
            finishDialog.data(),
            refreshPopup.data(),
            sleepPopup.data(),
            pageNavigationPopup.data(),
            tapZonesPopup.data(),
            navigationBarPopup.data(),
            menuViewPopup.data(),
            tappableLinksPopup.data(),
            marksActions.data(),
            removeDialog.data(),
            annotationExportPicker.data(),
            customReplaceDialog.data(),
            annotationErrorDialog.data(),
            formattingWarningDialog.data(),
            settingsWriteErrorDialog.data()};
}

bool Reader::popup(QObject *object) const {
    for (auto *widget : popups())
        if (widget && widget == object)
            return true;
    return false;
}

void Reader::raisePopups() {
    if (raisingPopups)
        return;
    raisingPopups = true;
    for (auto *widget : popups())
        if (widget && widget->isVisible())
            widget->raise();
    raisingPopups = false;
}

bool Reader::event(QEvent *event) {
    if (event->type() == QEvent::UpdateRequest && isVisible()) {
        // Only the plain reading bars can cross an unchanged text page
        // with a partial refresh. Other panels may contain color.
        setProperty("reader_plain_controls",
                    !appearanceEdit.panelOpen() && !general->isVisible() && !choices->isVisible() &&
                        !chapterGraph->isVisible() && !linkReturn->isVisible() && !dictionaryPanel->isVisible() &&
                        !searchPanel->isVisible() && !linksActive() && !lookupPending &&
                        !acknowledgments->isVisible() && !bookmarkList->isVisible() && !lightPopup->isVisible() &&
                        !busyNotice->isVisible() && !syncDialog && !kosyncDialog && !finishDialog && !annotationErrorDialog &&
                        !formattingWarningDialog && !settingsWriteErrorDialog && !services.suspended() &&
                        page->image.text("kobalt.animation_text_only") == "true");
        // These panels hold text and line controls. Changes inside them can
        // use a partial refresh; showing the whole panel keeps its policy.
        QWidget *partialPanel = nullptr;
        if (lightPopup->isVisible()) {
            partialPanel = lightPopup;
        } else if (appearanceEdit.panelOpen() && menu->isVisible() && !busyNotice->isVisible() &&
                   !choices->isVisible()) {
            partialPanel = menu;
        }
        setProperty("reader_partial_controls",
                    partialPanel ? QRect(partialPanel->mapTo(this, QPoint()), partialPanel->size()) : QRect());
    }
    return QWidget::event(event);
}

bool Reader::eventFilter(QObject *watched, QEvent *event) {
    const auto type = event->type();
    if (type == QEvent::TouchBegin || type == QEvent::TouchUpdate || type == QEvent::MouseButtonPress ||
        type == QEvent::KeyPress)
        lastInput.start();
    // Widgets that forward a press, such as a canvas, send a copy without the
    // input's time. Keep the time of the press the platform delivered.
    if ((type == QEvent::TouchBegin || type == QEvent::MouseButtonPress) && event->spontaneous())
        lastPressTime = quint32(static_cast<QInputEvent *>(event)->timestamp());
    if (watched == bookmarkList && type == QEvent::Hide)
        closeMarksActions();
    if (type == QEvent::Show && (watched == topControls || watched == general || watched == choices ||
                                 watched == dictionaryPanel || watched == searchPanel || watched == linkPanel ||
                                 watched == linkDialog || watched == syncDialog || watched == kosyncDialog || watched == finishDialog)) {
        book->pauseLocalPace();
    }
    if (watched == menu && type == QEvent::Hide && fieldChooser) {
        fieldChooser->hide();
        headerFooterView->hide();
    }
    if ((watched == brightnessNotice || watched == annotationModeNotice) && type == QEvent::Hide && isVisible() && !lifecycleHidden && !closing &&
        !exportRunning && !exitPending && !services.suspended()) {
        requestFlash(watched == brightnessNotice ? "light-swipe" : "annotation-mode");
        page->update();
    }
    // Kobo screens have no hover. Qt still sends hover events when a touch
    // moves between widgets, and repaints any widget that tracks hover, which
    // refreshes unchanged pixels. No reader widget draws a hover state.
    if (ui::isHoverEvent(event)) {
        auto *target = qobject_cast<QWidget *>(watched);
        if (target && (target == this || isAncestorOf(target)))
            return true;
    }
    if ((type == QEvent::KeyPress || type == QEvent::KeyRelease) && isVisible() && !lifecycleHidden &&
        !services.suspended() && QApplication::activeModalWidget() == this) {
        auto *key = static_cast<QKeyEvent *>(event);
        const int direction = services.pageButtonDirection(key);
        if (direction) {
            // Consume both events before a sheet's input barrier handles them.
            if (type == QEvent::KeyPress) {
                if (!closing && !book->deviceTransition() && services.remoteKeyActivity)
                    services.remoteKeyActivity(this, key);
                if (!key->isAutoRepeat())
                    pageButton(direction);
            }
            event->accept();
            return true;
        }
    }
    // While a panel's changes apply, its controls ignore input. Disabling
    // them instead would repaint the whole visible panel in disabled colors,
    // one more screen update before it closes.
    const bool controlInput = type == QEvent::MouseButtonPress || type == QEvent::MouseButtonRelease ||
                              type == QEvent::MouseButtonDblClick || type == QEvent::MouseMove ||
                              type == QEvent::TouchBegin || type == QEvent::TouchUpdate || type == QEvent::TouchEnd ||
                              type == QEvent::KeyPress || type == QEvent::KeyRelease || type == QEvent::Shortcut;
    if (controlInput && isVisible() && chromeView()) {
        auto *target = qobject_cast<QWidget *>(watched);
        if (target && (target == this || isAncestorOf(target))) {
            bool inPopup = false;
            for (auto *view : popups())
                if (view && view->isVisible() && (view == target || view->isAncestorOf(target))) {
                    inPopup = true;
                    break;
                }
            // Only the sheet and any confirmation above it accept reader
            // input. Host controls such as Screenshot remain independent.
            if (!inPopup) {
                event->accept();
                return true;
            }
        }
    }
    if (appearanceEdit.applying() && controlInput) {
        auto *target = qobject_cast<QWidget *>(watched);
        if (target &&
            (target == menu || target == general || menu->isAncestorOf(target) || general->isAncestorOf(target))) {
            event->accept();
            return true;
        }
    }
    // Reflow temporarily blocks rotation without repainting its buttons in
    // disabled colors. Keep the selected orientation visible until it changes.
    auto *rotationButton = qobject_cast<QPushButton *>(watched);
    if (controlInput && rotationButton &&
        (rotationButtons.contains(rotationButton) || rotationButton == autoRotateOption) && rotationControlsBlocked()) {
        event->accept();
        return true;
    }
    if (watched == menu && type == QEvent::Paint) {
        schedulePreviewWarm();
    }
    // Panels declare their placement through geometry. The reading status strip
    // owns its separator, including when an existing panel moves or closes.
    if (watched->parent() == this && dynamic_cast<ui::Panel *>(watched) &&
        (type == QEvent::Show || type == QEvent::Hide || type == QEvent::Move || type == QEvent::Resize) &&
        !statusBoundaryPending) {
        statusBoundaryPending = true;
        QTimer::singleShot(0, this, [this] {
            statusBoundaryPending = false;
            syncStatusBoundary();
        });
    }
    // The overlay itself only receives taps outside the refresh card.
    if (refreshPopup && watched == refreshPopup && type == QEvent::MouseButtonRelease) {
        closeScreenRefresh();
        return true;
    }
    if (sleepPopup && watched == sleepPopup && type == QEvent::MouseButtonRelease) {
        closeSleepScreen();
        return true;
    }
    if (tapZonesPopup && watched == tapZonesPopup && type == QEvent::MouseButtonRelease) {
        closeTapZones();
        return true;
    }
    if (navigationBarPopup && watched == navigationBarPopup && type == QEvent::MouseButtonRelease) {
        closeNavigationBarButtons();
        return true;
    }
    if (tappableLinksPopup && watched == tappableLinksPopup && type == QEvent::MouseButtonRelease) {
        closeTappableLinks();
        return true;
    }
    if (menuViewPopup && watched == menuViewPopup && type == QEvent::MouseButtonRelease) {
        closeMenuView();
        return true;
    }
    if (pageNavigationPopup && watched == pageNavigationPopup && type == QEvent::MouseButtonRelease) {
        closePageNavigation();
        return true;
    }
    // Panels are raised as they open, and a panel created later starts
    // above older widgets. Either can cover a centred pop-up, so put the
    // visible pop-ups back on top whenever another child moves up.
    if ((type == QEvent::ZOrderChange || type == QEvent::Show) && watched->parent() == this && !popup(watched))
        raisePopups();
    const bool touch = type == QEvent::TouchBegin || type == QEvent::TouchUpdate || type == QEvent::TouchEnd ||
                       type == QEvent::TouchCancel;
    const bool mouse = type == QEvent::MouseButtonPress || type == QEvent::MouseButtonDblClick ||
                       type == QEvent::MouseMove || type == QEvent::MouseButtonRelease;
    if (!touch && !mouse)
        return QWidget::eventFilter(watched, event);
    bool &dismissing = touch ? dismissingFontTouch : dismissingFontMouse;
    const bool begin =
        type == QEvent::TouchBegin || type == QEvent::MouseButtonPress || type == QEvent::MouseButtonDblClick;
    if (begin) {
        dismissing = false;
        if (isVisible() && lightPopup->isVisible()) {
            QPointF point;
            if (touch) {
                const auto points = kobalt::ui::touchPositions(static_cast<QTouchEvent *>(event), true);
                if (points.isEmpty())
                    return QWidget::eventFilter(watched, event);
                point = points.first();
            } else
                point = kobalt::ui::mousePosition(static_cast<QMouseEvent *>(event), true);
            const QRect panelRect(lightPopup->mapToGlobal(QPoint()), lightPopup->size());
            const QRect toolbarRect(topControls->mapToGlobal(QPoint()), topControls->size());
            if (!panelRect.contains(point.toPoint()) && !toolbarRect.contains(point.toPoint()) &&
                !(navigation && navigation->contains(point.toPoint()))) {
                dismissing = true;
                lightPopup->hide();
                showControls();
            }
        }
        if (isVisible() && choosingTypography && choices->isVisible() && !fontDefaultDialog) {
            QPointF point;
            if (touch) {
                const auto points = kobalt::ui::touchPositions(static_cast<QTouchEvent *>(event), true);
                if (points.isEmpty())
                    return QWidget::eventFilter(watched, event);
                point = points.first();
            } else
                point = kobalt::ui::mousePosition(static_cast<QMouseEvent *>(event), true);
            const auto openingMenu = [&](const char *name) {
                const auto *button = findChild<QPushButton *>(name);
                return button->isVisible() && button->isEnabled() &&
                       QRect(button->mapToGlobal(QPoint()), button->size()).contains(point.toPoint());
            };
            if (!openingMenu("rrLight") && !openingMenu("rrSettings") &&
                !QRect(choices->mapToGlobal(QPoint()), choices->size()).contains(point.toPoint())) {
                dismissing = true;
                choices->hide();
                refresh();
            }
        }
    }
    if (!dismissing)
        return QWidget::eventFilter(watched, event);
    // Consume the whole gesture so an outside tap cannot also turn a page
    // or apply the draft through the button underneath the dropdown.
    if (type == QEvent::TouchEnd || type == QEvent::TouchCancel || type == QEvent::MouseButtonRelease)
        dismissing = false;
    event->accept();
    return true;
}

bool Reader::readyForPresentation() const {
    const QSize viewport(width(), qMax(1, height() - page->topHeight() - page->bottomHeight()));
    return isVisible() && updatesEnabled() && !resizePending && !model->busy() && !page->image.isNull() &&
           page->image.size() == viewport &&
           (model->resourceStats().value("pending_images").toInt() == 0 || !model->error().isEmpty());
}

void Reader::setOpeningTiming(const QElapsedTimer &started, const QJsonObject &stages) {
    openingStarted = started;
    // Showing starts when the host hands the window over, so the host's own
    // preparation of the screen counts as part of it.
    showing.start();
    for (auto it = stages.begin(); it != stages.end(); ++it)
        openingStages[it.key()] = it.value();
}
void Reader::reportShown() {
    if (!kobalt::diagnosticsActive())
        return;
    auto fields = openingStages;
    fields["show_ms"] = showing.nsecsElapsed() / 1e6;
    if (openingStarted.isValid())
        fields["total_ms"] = openingStarted.nsecsElapsed() / 1e6;
    kobalt::diagnostic("reader-shown", fields, false);
}

void Reader::locateText(const QString &text, std::function<void(QPoint)> found) {
    auto settled = std::make_shared<bool>(false);
    QPointer<Reader> guard(this);
    model->search(text, [guard, text, found, settled](kobalt::SearchBatch batch) {
        if (!guard || *settled)
            return;
        // A hit inside a longer word, such as "set" in "reset", does not count.
        const auto joins = [](const QString &side, bool last) {
            return !side.isEmpty() && side.at(last ? side.size() - 1 : 0).isLetterOrNumber();
        };
        for (const auto &hit : batch.hits) {
            if (hit.text.compare(text, Qt::CaseInsensitive) != 0 || joins(hit.before, true) || joins(hit.after, false))
                continue;
            *settled = true;
            guard->model->cancelSearch();
            if (hit.page != guard->model->page())
                guard->model->goTo(hit.start);
            guard->locateOnPage(hit, found, 0);
            return;
        }
        if (batch.done) {
            *settled = true;
            found({-1, -1});
        }
    });
}
// The turn to the hit's page renders first; its rectangles exist only then.
void Reader::locateOnPage(const kobalt::SearchHit &hit, std::function<void(QPoint)> found, int attempt) {
    if (model->busy() || model->page() != hit.page) {
        if (attempt >= 500) {
            found({-1, -1});
            return;
        }
        QPointer<Reader> guard(this);
        kobalt::qt::later(20, this, [guard, hit, found, attempt] {
            if (guard)
                guard->locateOnPage(hit, found, attempt + 1);
        });
        return;
    }
    QPointer<Reader> guard(this);
    model->rangeRects(hit.start, hit.end, [guard, found](QVector<QRect> rectangles) {
        if (!guard || rectangles.isEmpty())
            found({-1, -1});
        else
            found(rectangles.first().center() + QPoint(0, guard->page->topHeight()));
    });
}
void Reader::locateLink(const QString &label, std::function<void(QPoint)> found) {
    findLink(model->pageLinks(), 0, label, std::move(found));
}
void Reader::findLink(QVector<kobalt::PageLink> links, int index, QString label, std::function<void(QPoint)> found) {
    if (index >= links.size()) {
        found({-1, -1});
        return;
    }
    const auto link = links.at(index);
    QPointer<Reader> guard(this);
    model->linkDetails(link.source, [guard, links, index, label, found, link](kobalt::LinkDetails details) {
        if (!guard)
            return;
        if (details.label == label && !link.rectangles.isEmpty())
            found(link.rectangles.first().center() + QPoint(0, guard->page->topHeight()));
        else
            guard->findLink(links, index + 1, label, found);
    });
}

void Reader::showEvent(QShowEvent *event) {
    kobalt::diagnostic("reader-visible");
    // The first show ends the opening once the page has painted.
    if (!shownOnce) {
        shownOnce = true;
        if (!showing.isValid())
            showing.start();
        kobalt::ui::afterNextPaint(page, [this] {
            pagePainted = true;
            reportShown();
            if (!recordedOpening)
                recordOpening();
        });
    }
    // A modal window still lets Nickel repaint its library behind us.
    // Those background flushes can request GC16 over our reading page.
    presentation->pauseBackground();
    if (orientation)
        orientation->start(kobalt::shownOrientation(QGuiApplication::primaryScreen()));
    QWidget::showEvent(event);
    resizeText();
    observeReading();
    if (clockTimer)
        startClock();
    // The first refresh runs before the window is visible. Record the
    // opening once it is and its page has painted; a busy model retries from
    // its next refresh.
    if (!recordedOpening)
        QTimer::singleShot(0, this, [this] {
            if (!recordedOpening && pagePainted)
                recordOpening();
        });
    else
        scheduleNativeOpeningSave();
}

void Reader::hideEvent(QHideEvent *event) {
    if (nativeOpeningSaveTimer)
        nativeOpeningSaveTimer->stop();
    if (clockTimer)
        clockTimer->stop();
    if (pageScrubber)
        pageScrubber->cancel();
    renderNoticeTimer.stop();
    busyNotice->setDelayedProgress(false, 0, 0);
    model->cancelPendingTurn();
    book->stopReading();
    appearanceEdit.clearContinuation();
    closeSearch();
    services.cancelAnimation();
    page->animationDirection = 0;
    setProperty("reader_animation_direction", 0);
    dismissLookup();
    kobalt::diagnostic("reader-hidden");
    if (light)
        lightPopup->hide();
    if (orientation) {
        orientation->stop(property("reader_sleep_handoff").toBool());
    }
    presentation->resumeBackground();
    resizePending = false;
    setUpdatesEnabled(true);
    QWidget::hideEvent(event);
}

void Reader::resizeEvent(QResizeEvent *) {
    annotationFrame.clear();
    if (annotationPopover)
        annotationPopover->hide();
    if (isVisible() && windowHandle() && windowHandle()->isExposed() && !page->image.isNull() && updatesEnabled()) {
        // Kobo Qt replaces the backing store when the screen turns. Keep
        // painting the current page until reflow finishes, or the new buffer
        // reaches the panel as black while the engine is busy.
        resizePending = true;
        const auto viewport = QSize(width(), qMax(1, height() - page->topHeight() - page->bottomHeight()));
        kobalt::diagnostic("reader-resize-begin",
                           {{"width", width()},
                            {"height", height()},
                            {"viewport_width", viewport.width()},
                            {"viewport_height", viewport.height()},
                            {"main_busy", model->busy()},
                            {"preview_busy", appearanceEdit.preview() && appearanceEdit.preview()->busy()},
                            {"updates_enabled", updatesEnabled()},
                            {"resize_pending", resizePending}});
    }
    lightPopup->hide();
    dismissLookup();
    if (marksActions) {
        marksActions->setGeometry(rect());
        marksActions->fitCard();
    }
    header->setGeometry(0, 0, width(), page->topHeight());
    header->setVisible(page->topHeight() > 0);
    topControls->setGeometry(0, 0, width(), navigation ? navigation->toolbarHeight() : m::panel::toolbarHeight);
    bottomControls->setGeometry(m::panel::footer(size()));
    fitAppearance();
    chapterGraph->resize(width(), chapterGraph->height());
    choices->setGeometry(rect());
    fitSettings();
    acknowledgments->setGeometry(rect());
    searchPanel->setGeometry(rect());
    const int bookmarkTop = navigation ? navigation->statusHeight() : 0;
    bookmarkList->setGeometry(0, bookmarkTop, width(),
                              (navigation ? navigation->contentBottom(size()) : height()) - bookmarkTop);
    busyNotice->setGeometry(rect());
    busyNotice->card()->fitToWidth(qMin<int>(m::notice::cardWidth, width() - 2 * m::notice::sideClearance));
    busyNotice->centerCard();
    brightnessNotice->centerIn(size());
    annotationModeNotice->centerIn(size());
    if (syncDialog)
        syncDialog->setGeometry(rect());
    if (kosyncDialog)
        kosyncDialog->setGeometry(rect());
    if (finishDialog)
        finishDialog->setGeometry(rect());
    resizeText();
    syncNavigation();
}

void Reader::closeEvent(QCloseEvent *event) {
    model->cancelPendingTurn();
    appearanceEdit.clearContinuation();
    closeSearch();
    dismissLookup();
    if (light)
        lightPopup->hide();
    if (!closing) {
        kobalt::diagnostic("exit-requested");
        if (!closingTimes.started.isValid())
            closingTimes.started.start();
        event->ignore();
        exitPending = true;
        QElapsedTimer saving;
        saving.start();
        save(true);
        closingTimes.add("checkpoint", saving);
        if (appearanceEdit.panelOpen() || general->isVisible())
            dismissControls();
        finishExit();
        return;
    }
    presentation->saveSleepScreen(exitSleepSnapshot);
    exitSleepSnapshot = {};
    setProperty("reader_exit_clean_pending", true);
    setProperty("reader_preparing_handoff", true);
    // Restore orientation while Home is paused, then repaint its backing store
    // before making it visible.
    if (orientation) {
        orientation->stop();
    }
    presentation->resumeAndRepaintBackground();
    setProperty("reader_preparing_handoff", false);
    QElapsedTimer step;
    step.start();
    book->releaseNative();
    closingTimes.add("session", step);
    step.restart();
    save(true);
    closingTimes.add("checkpoint", step);
    model->updated = {};
    model->renderProgressChanged = {};
    model->turnBoundary = {};
    renderNoticeTimer.stop();
    busyNotice->setDelayedProgress(false, 0, 0);
    if (finished)
        finished(model->retained());
    QWidget::closeEvent(event);
}

bool Reader::rotationControlsBlocked() const {
    return model->busy() || (appearanceEdit.edited() && appearanceEdit.preview()->busy());
}

void Reader::updateOrientationBlock() {
    if (orientation)
        orientation->setBlocked(lifecycleHidden || services.suspended() || topControls->isVisible() || model->busy() ||
                                appearanceEdit.panelOpen() || bool(syncDialog) || bool(kosyncDialog) || bool(finishDialog) ||
                                bool(annotationErrorDialog) || bool(formattingWarningDialog) ||
                                bool(settingsWriteErrorDialog) || dictionaryPanel->isVisible() ||
                                searchPanel->isVisible() || linksActive() || lookupPending);
}

void Reader::closeSearch() {
    ++searchEpoch;
    pendingSearchHit = {};
    model->cancelSearch();
    if (searchPanel) {
        searchPanel->stop();
        searchPanel->hide();
    }
    updateOrientationBlock();
    syncNavigation();
}

void Reader::openSeek() {
    if (model->busy() || appearanceEdit.applying()) {
        return;
    }
    if (appearanceEdit.panelOpen() || general->isVisible()) {
        appearanceEdit.request(AppearanceDestination::Seek);
        if (general->isVisible()) {
            applySettings();
        } else {
            finishPreview();
        }
        return;
    }
    if (seekOpen) {
        if (pageNavigation::menuView(preferences) == pageNavigation::MenuView::Seek)
            return;
        seekOpen = false;
        pageScrubber->cancel();
        showControls();
        return;
    }
    choices->hide();
    lightPopup->hide();
    chapterGraph->hide();
    seekOpen = true;
    updateLinkReturn();
    pageScrubber->showCurrentPage();
}

void Reader::openSearch() {
    model->cancelPendingTurn();
    if (model->busy() || appearanceEdit.panelOpen() || appearanceEdit.applying())
        return;
    dismissLookup();
    dismissControls();
    if (searchOriginal.isEmpty()) {
        searchOriginal = model->state().value("position").toString();
        searchOriginalPage = model->page();
    }
    searchPanel->setExcerptFont(bookFace());
    searchPanel->setChapters(model->chapters());
    searchPanel->setGeometry(rect());
    searchPanel->open(model->state().value("position").toString() != searchOriginal, searchOriginalPage);
    if (orientation)
        orientation->setBlocked(true);
    for (auto *widget : searchPanel->findChildren<QWidget *>())
        textRefresh(widget);
    textRefresh(searchPanel);
}

bool Reader::linksActive() const {
    return linkPending || bool(linkDialog) || (linkPanel && linkPanel->isVisible());
}

void Reader::dismissLinkConfirmation() {
    if (linkDialog) {
        linkDialog->hide();
        linkDialog->deleteLater();
        linkDialog.clear();
    }
}

void Reader::dismissLinks() {
    ++linkEpoch;
    linkPending = false;
    if (linkPanel) {
        const bool wasVisible = linkPanel->isVisible();
        linkPanel->hide();
        if (wasVisible) {
            requestFlash("footnote");
            page->update();
        }
    }
    dismissLinkConfirmation();
}

void Reader::updateLinkReturn() {
    if (!linkReturn)
        return;
    syncLinkDiscovery();
    const bool available = !linkHistory.isEmpty() && !model->busy() && !lifecycleHidden && !closing && !linksActive() &&
                           !lookupPending && !general->isVisible() && !acknowledgments->isVisible() &&
                           !bookmarkList->isVisible() && !lightPopup->isVisible() && !syncDialog && !kosyncDialog && !finishDialog &&
                           (bottomControls->isVisible() ||
                            (topControls->isVisible() && !menu->isVisible() && !chapterGraph->isVisible())) &&
                           !choices->isVisible() && !searchPanel->isVisible() && !dictionaryPanel->isVisible();
    linkReturnAvailable = available;
    linkReturn->setVisible(available);
    const int footerHeight = m::panel::footerHeight + (available ? m::links::returnHeight + m::panel::footerGap : 0);
    bottomControls->setGeometry(0, height() - footerHeight, width(), footerHeight);
    syncNavigation();
}

bool Reader::openLink(QPoint point, bool direct) {
    if (model->busy() || closing || lifecycleHidden || services.suspended() || syncDialog || kosyncDialog || finishDialog)
        return false;
    for (const auto &link : model->pageLinks()) {
        if (direct && !link.tappable(preferences.value("tappable_links").toString("off")))
            continue;
        // Ordinary reading keeps nearby text available to the page tap zones.
        const int pad = direct ? 0 : kobalt::ui::px(6);
        QRect bounds;
        for (const auto &rect : link.rectangles) {
            if (rect.adjusted(-pad, -pad, pad, pad).contains(point)) {
                bounds = rect;
                break;
            }
        }
        if (bounds.isEmpty())
            continue;
        dismissLookup();
        linkPending = true;
        // A link chosen in reveal mode opens like a held link, over the page
        // alone. Restoring the toolbars would cover the link's sheet.
        hideRevealedLinks(false);
        syncNavigation();
        const auto epoch = ++linkEpoch;
        if (orientation)
            orientation->setBlocked(true);
        page->mark(link.rectangles);
        QPointer<Reader> guard(this);
        kobalt::ui::PanelTiming timing("footnote");
        model->linkDetails(link.source, [guard, epoch, bounds, timing](const kobalt::LinkDetails &details) mutable {
            if (!guard || guard->linkEpoch != epoch || guard->lifecycleHidden || guard->services.suspended())
                return;
            timing.stage("details");
            auto *reader = guard.data();
            reader->linkPending = false;
            if (details.kind == kobalt::LinkKind::None) {
                reader->dismissLookup();
                reader->updateLinkReturn();
                return;
            }
            if (details.kind == kobalt::LinkKind::Passage) {
                reader->goToPassage(details.target);
            } else if (details.kind == kobalt::LinkKind::Website) {
                auto *dialog = new kobalt::WebsiteDialog(reader);
                reader->linkDialog = dialog;
                dialog->browserAvailable = [reader] {
                    return reader->services.browserAvailable();
                };
                dialog->openBrowser = [reader](const QUrl &url) {
                    return !reader->closing && !reader->lifecycleHidden && !reader->services.suspended() &&
                           reader->services.openBrowser(url, reader);
                };
                dialog->dismissed = [reader] {
                    reader->dismissLookup();
                    reader->updateLinkReturn();
                };
                dialog->open(details.target);
                textRefresh(dialog);
            } else {
                reader->linkPanel->open(
                    details, bounds.translated(0, reader->page->topHeight()),
                    reader->rect().adjusted(0, reader->page->topHeight(), 0, -reader->page->bottomHeight()));
                timing.finish(reader->linkPanel);
                textRefresh(reader->linkPanel);
            }
            reader->updateOrientationBlock();
            reader->updateLinkReturn();
        });
        return true;
    }
    return false;
}

void Reader::goToPassage(const QString &target) {
    if (target.isEmpty() || model->busy() || closing || lifecycleHidden || services.suspended()) {
        return;
    }
    const LinkVisit origin{
        model->state().value("position").toString(), model->page(),
        (model->chapterTitle().isEmpty() ? strings::get("reader.reading") : model->chapterTitle()).toUpper()};
    if (!model->goTo(target)) {
        return;
    }
    pendingPassage = origin;
    dismissLookup();
    dismissControls();
    refresh();
}

void Reader::dismissLookup(bool keepGesture) {
    annotationModeHide.stop();
    if (annotationModeNotice)
        annotationModeNotice->hide();
    dismissLinks();
    ++lookupEpoch;
    lookupPending = false;
    if (keepGesture)
        page->stopHold();
    else
        page->cancelHold();
    brightnessHide.stop();
    if (brightnessNotice)
        brightnessNotice->hide();
    page->mark({});
    if (dictionaryPanel) {
        const bool wasVisible = dictionaryPanel->isVisible();
        dictionaryPanel->hide();
        if (wasVisible) {
            requestFlash("dictionary");
            page->update();
        }
    }
    updateOrientationBlock();
}

void Reader::dismissDictionary() {
    if (annotationMode)
        dismissLookup();
    else
        clearAnnotationDraft();
    updateLinkReturn();
}

void Reader::lookupWord(QPoint point) {
    if (beginAnnotationDrag(point, -1))
        finishAnnotationDrag(false);
}

void Reader::lookupSelection() {
    if (annotationStart.isEmpty() || annotationEnd.isEmpty())
        return;
    const auto epoch = ++lookupEpoch;
    lookupPending = true;
    updateOrientationBlock();
    updateLinkReturn();
    QPointer<Reader> guard(this);
    kobalt::ui::PanelTiming timing("dictionary");
    model->textRange(annotationStart, annotationEnd, [guard, epoch, timing](TextRange range) mutable {
        if (!guard || guard->lookupEpoch != epoch || guard->lifecycleHidden || guard->services.suspended())
            return;
        timing.stage("range");
        guard->lookupPending = false;
        const auto query = range.text.simplified();
        if (query.isEmpty()) {
            guard->updateOrientationBlock();
            guard->updateLinkReturn();
            return;
        }
        QRect bounds;
        for (const auto &rect : guard->draftHighlightRects)
            bounds = bounds.united(rect);
        guard->lookupText = query;
        guard->lookupBounds = bounds.translated(0, guard->page->topHeight());
        const auto result = guard->dictionary->lookup(guard->book->identity(), query);
        if (!guard || guard->lookupEpoch != epoch || guard->lifecycleHidden)
            return;
        timing.stage("lookup");
        timing.field("chars", result.html.size());
        guard->dictionaryPanel->setChoices(guard->dictionary->choices());
        guard->dictionaryPanel->setBookFont(guard->bookFace());
        guard->dictionaryPanel->open(query, result, guard->lookupBounds);
        timing.finish(guard->dictionaryPanel);
        guard->syncAnnotationControls();
        guard->updateOrientationBlock();
        guard->updateLinkReturn();
    });
}

// Wake at the next wall-clock minute or the next reading minute of the
// menu status, whichever comes first.
void Reader::startClock() {
    const auto clock = 60000 - QTime::currentTime().msecsSinceStartOfDay() % 60000;
    // A stopped reading clock never reaches its next minute.
    const auto reading = book->readingActive() ? 60000 - book->readingMilliseconds() % 60000 : 60000;
    clockTimer->start(int(qMin<qint64>(clock, reading)) + 50);
}

void Reader::configureNavigation() {
    navigation = std::make_unique<kobalt::reader::Navigation>(this, toolbarComposition);
    navigation->setScrubber(pageScrubber);
    navigation->setStatusVisible(!hideNativeStatus());
    chapterGraph->setTopPadding(navigation->statsTopPadding());
    topControls->setGeometry(0, 0, width(), navigation->toolbarHeight());
    topControls->layout()->activate();
    for (auto *panel : {static_cast<kobalt::ui::Panel *>(menu), static_cast<kobalt::ui::Panel *>(chapterGraph),
                        static_cast<kobalt::ui::Panel *>(lightPopup)})
        panel->setEdges(navigation->panelEdge());
    navigation->returnRequested = [this] {
        linkReturn->click();
    };
    navigation->stayRequested = [this] {
        linkHistory.clear();
        updateLinkReturn();
        syncNavigation();
    };
    navigation->activated = [this](kobalt::NavigationDestination destination) {
        activateNavigation(destination);
    };
}

void Reader::activateNavigation(kobalt::NavigationDestination destination) {
    model->cancelPendingTurn();
    bookmarkList->hide();
    using Nav = kobalt::NavigationDestination;
    if (destination != Nav::Seek) {
        pageScrubber->cancel();
    }
    if (destination == Nav::Seek)
        openSeek();
    else if (destination == Nav::Text)
        selectPanel(0);
    else if (destination == Nav::Layout)
        selectPanel(1);
    else if (destination == Nav::Light)
        toggleLight();
    else if (destination == Nav::Stats)
        toggleGraph();
    else {
        const auto target =
            destination == Nav::Chapters ? AppearanceDestination::Chapters : AppearanceDestination::Search;
        if (target == AppearanceDestination::Chapters && choices->isVisible() && !choosingTypography) {
            choices->hide();
            showControls();
        } else if (appearanceEdit.panelOpen() || general->isVisible()) {
            appearanceEdit.request(target);
            if (general->isVisible())
                applySettings();
            else
                finishPreview();
        } else if (target == AppearanceDestination::Chapters)
            showChapters();
        else
            openSearch();
    }
    syncNavigation();
}

QRect Reader::panelBounds(int panelHeight) const {
    return navigation ? navigation->panelBounds(size(), panelHeight)
                      : QRect(0, m::panel::toolbarHeight, width(), panelHeight);
}

void Reader::syncStatusBoundary() {
    const int boundary = page->topHeight();
    bool adjacent = false;
    for (auto *child : children()) {
        auto *panel = dynamic_cast<ui::Panel *>(child);
        if (panel && panel->isVisible() && panel->y() == boundary && panel->x() == 0 && panel->width() == width()) {
            adjacent = true;
            break;
        }
    }
    page->setPanelBelowHeader(boundary > 0 && adjacent);
}

void Reader::hideRevealedLinks(bool restoreControls) {
    if (revealingLinks && restoreControls && !lifecycleHidden && !closing) {
        topControls->show();
        bottomControls->show();
    }
    revealingLinks = false;
    revealedPosition.clear();
    page->revealLinks({});
}

void Reader::showRevealedLinks() {
    model->cancelPendingTurn();
    revealingLinks = true;
    revealedPosition = model->state().value("position").toString();
    // Reveal mode must expose links beneath the ordinary toolbars too.
    topControls->hide();
    bottomControls->hide();
    syncNavigation();
}

void Reader::leaveRevealedLinks() {
    dismissControls();
}

void Reader::syncLinkDiscovery() {
    if (!linkDiscovery)
        return;
    const bool blocked = annotationMode || model->busy() || lifecycleHidden || closing || services.suspended() ||
                         appearanceEdit.panelOpen() || general->isVisible() || menu->isVisible() ||
                         choices->isVisible() || chapterGraph->isVisible() || lightPopup->isVisible() ||
                         bookmarkList->isVisible() || acknowledgments->isVisible() || searchPanel->isVisible() ||
                         dictionaryPanel->isVisible() || linksActive() || lookupPending || syncDialog || kosyncDialog || finishDialog;
    if (blocked || (revealingLinks && revealedPosition != model->state().value("position").toString()))
        hideRevealedLinks();
    const int count = model->pageLinks().size();
    if (!count)
        hideRevealedLinks();
    const bool controls = topControls->isVisible() || revealingLinks;
    const bool linksAvailable = !blocked && controls && count > 0;
    pageScrubber->setLinksAction(linksAvailable && !revealingLinks ? strings::get("reader.highlight_links").arg(count)
                                                                   : QString());
    linkDiscovery->setVisible(linksAvailable && revealingLinks);
    linkIndicator->setVisible(!blocked && !controls && count > 0 &&
                              preferences.value("show_link_indicator").toBool(false));
    QVector<QRect> rectangles;
    if (revealingLinks)
        for (const auto &link : model->pageLinks())
            rectangles += link.rectangles;
    page->revealLinks(rectangles);
    if (blocked || !count)
        return;

    namespace styles = kobalt::ui::styles;
    const QColor ink = palette().color(QPalette::WindowText);
    // Styles change only with the place and the strip text size, so skip
    // unchanged ones: this runs on every navigation update.
    const auto restyle = [](QWidget *widget, const QString &style) {
        if (widget->property("link_style").toString() == style)
            return;
        widget->setProperty("link_style", style);
        kobalt::ui::readerStyle(widget, style);
    };
    // Strip buttons keep the strip's height as their touch area and draw a
    // compact outlined box inside it, in the strip's text size.
    const auto stripButton = [&](QPushButton *button, int strip, int text) {
        const int box =
            qMin(strip - 2 * int(m::links::stripButtonInset),
                 ui::interfaceLineHeight(ui::interfaceFont(text)) + 2 * int(m::links::stripButtonInset));
        restyle(button, styles::outlined() + QString("QPushButton{%1min-height:0;padding:0 %2px;margin:%3px 0;}")
                                                 .arg(styles::font(text))
                                                 .arg(int(m::links::stripButtonPadding))
                                                 .arg(qMax(0, (strip - box) / 2)));
        button->setFixedHeight(strip);
    };
    const auto fits = [](int strip, int text) {
        return strip >= ui::interfaceLineHeight(ui::headerFooterFont(text));
    };

    const QString label = strings::get("reader.highlight_links").arg(count);
    if (linkIndicator->isVisible()) {
        // The indicator sits at the right of the footer, level with the page
        // number. Without a footer it stays just above the bottom edge.
        QRect strip = page->footerTextRect();
        if (strip.isEmpty())
            strip = QRect(0, height() - page->bottomHeight() - m::reading::stripHeight(page->chrome.footerSize),
                          width(), m::reading::stripHeight(page->chrome.footerSize));
        strip.translate(page->mapTo(this, QPoint()));
        const int text = page->footerTextPixels();
        linkIndicator->setText(QString::number(count));
        linkIndicator->setAccessibleName(label);
        linkIndicator->setToolTip(label);
        linkIndicator->setIcon(kobalt::ui::controlIcon(kobalt::ui::ControlIcon::Link, ink));
        linkIndicator->setIconSize(QSize(text, text));
        stripButton(linkIndicator, strip.height(), text);
        linkIndicator->adjustSize();
        linkIndicator->move(strip.right() + 1 - m::reading::headerSide - linkIndicator->width(), strip.y());
        linkIndicator->raise();
    }

    auto *row = static_cast<QHBoxLayout *>(linkDiscovery->layout());
    linkDiscoveryIcon->setVisible(!revealingLinks);
    linkDiscoveryToggle->setVisible(!revealingLinks);
    linkDiscoveryExit->setVisible(revealingLinks);
    linkDiscoveryHint->setText(revealingLinks ? strings::get("reader.tap_revealed_link").toUpper()
                                              : strings::get("reader.long_press_link"));
    // In reveal mode the bar only labels the mode. Taps pass to the page, so
    // anything but a link leaves, the strip included.
    linkDiscovery->setAttribute(Qt::WA_TransparentForMouseEvents, revealingLinks);
    linkDiscoveryHint->setWordWrap(!revealingLinks);
    linkDiscoveryHint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    if (revealingLinks) {
        // The label replaces the header or footer strip, never the book text,
        // in the strip's own face and size. Without a strip that fits, a tap on
        // unlinked text still leaves.
        const bool top = fits(page->topHeight(), page->headerTextPixels());
        const int strip = top ? page->topHeight() : page->bottomHeight();
        const int text = top ? page->headerTextPixels() : page->footerTextPixels();
        linkDiscovery->setVisible(top || fits(strip, text));
        linkDiscovery->setEdges({});
        linkDiscovery->setDividers(top ? Qt::BottomEdge : Qt::TopEdge);
        // Children paint their own background, so keep them off the divider.
        const int border = m::control::border;
        row->setContentsMargins(m::reading::headerSide, top ? 0 : border, m::reading::headerSide, top ? border : 0);
        const QString stripText = QString("QLabel{font-family:'%1';%2color:%3;}")
                                      .arg(kobalt::ui::headerFooterFont().family())
                                      .arg(styles::font(text))
                                      .arg(ink.name());
        restyle(linkDiscoveryHint, stripText);
        restyle(linkDiscoveryExit, stripText);
        linkDiscovery->setGeometry(0, top ? 0 : height() - strip, width(), strip);
    } else {
        // Above the tabs the row matches the return row: the same height, the
        // icon in the same column and a divider instead of a second heavy rule.
        const int rule = m::chrome::heavyRule;
        linkDiscovery->setEdges(Qt::TopEdge);
        linkDiscovery->setDividers(navigation ? Qt::BottomEdge : kobalt::ui::Edges());
        row->setContentsMargins(m::chrome::inset, rule, m::chrome::inset, m::control::border);
        linkDiscoveryIcon->setPixmap(kobalt::ui::controlIcon(kobalt::ui::ControlIcon::Link, ink)
                                         .pixmap(m::control::iconSize, m::control::iconSize));
        restyle(linkDiscoveryHint,
                QString("QLabel{%1color:%2;}").arg(styles::font(m::chrome::labelText)).arg(ink.name()));
        restyle(linkDiscoveryToggle, styles::outlined() + "QPushButton{" + styles::font(m::chrome::labelText) + "}");
        linkDiscoveryToggle->setText(label);
        // Cover the navigation's own heavy rule, so its tabs start below this divider.
        const int bottom = navigation ? navigation->navigationTop() + rule : bottomControls->y();
        const int rowHeight = m::links::returnHeight + rule;
        linkDiscovery->setGeometry(0, qMax(0, bottom - rowHeight), width(), rowHeight);
    }
    if (linkDiscovery->isVisible())
        linkDiscovery->raise();
}

void Reader::syncNavigation() {
    updateKosyncToolbar();
    syncStatusBoundary();
    if (navigation) {
        navigation->setStatusVisible(!hideNativeStatus());
    }
    syncLinkDiscovery();
    if (!navigation)
        return;
    if (topControls->isVisible()) {
        auto *settings = appearanceEdit.edited() && appearanceEdit.ready() ? appearanceEdit.preview() : model;
        QString face = settings->fontFace();
        if (face.isEmpty())
            face = settings->state().value("text_font_face").toString();
        if (face.isEmpty())
            face = QString::fromLatin1(kobalt::defaultFontFace);
        if (face != navigationFontFace) {
            navigationFontFace = face;
            bool loadable = false;
            for (const auto &font : model->fontCatalog())
                loadable |= font.family == face;
            if (!loadable)
                face = QString::fromLatin1(kobalt::defaultFontFace);
            registerPickerFonts(face);
            navigation->setTextFace(face);
        }
    }
    const bool chapters = choices->isVisible() && !choosingTypography;
    // The field list takes the place Chapters takes, under the Layout tab.
    // Header and footer is a sheet over the page, placed like Font below.
    QWidget *cover = fieldChooser && fieldChooser->isVisible() ? fieldChooser : nullptr;
    const bool chromeSheet = !cover && headerFooterView && headerFooterView->isVisible();
    using Nav = kobalt::NavigationDestination;
    const auto update = [&] {
        const int active = cover || chromeSheet                                                    ? int(Nav::Layout)
                           : chapters                                                              ? int(Nav::Chapters)
                           : choosingTypography && choices->isVisible() && choices->isFontPicker() ? int(Nav::Text)
                           : menu->isVisible()       ? (panels->currentIndex() == 0 ? int(Nav::Text) : int(Nav::Layout))
                           : lightPopup->isVisible() ? int(Nav::Light)
                           : chapterGraph->isVisible() ? int(Nav::Stats)
                           : seekOpen                  ? int(Nav::Seek)
                                                       : -1;
        pageScrubber->setReturnAction(
            linkReturnAvailable ? strings::get("reader.return_page").arg(linkHistory.last().page + 1) : QString(),
            linkHistory.isEmpty() || linkHistory.last().chapter.isEmpty()
                ? QString()
                : strings::get("reader.jumped_from_chapter").arg(linkHistory.last().chapter));
        navigation->update(
            size(),
            {bottomControls, menu, chapterGraph,
             cover                       ? cover
             : bookmarkList->isVisible() ? static_cast<QWidget *>(bookmarkList)
                                         : static_cast<QWidget *>(choices),
             cover || chapters || bookmarkList->isVisible(),
             topControls->isVisible() && !searchPanel->isVisible() && !acknowledgments->isVisible(),
             !model->busy() && !appearanceEdit.applying(), services.lightAvailable(), active,
             linkReturnAvailable ? strings::get("reader.return_page").arg(linkHistory.last().page + 1) : QString(),
             topActions, general, lightPopup,
             linkHistory.isEmpty() || linkHistory.last().chapter.isEmpty()
                 ? QString()
                 : strings::get("reader.jumped_from_chapter").arg(linkHistory.last().chapter),
             seekOpen,
             pageNavigation::menuView(preferences) == pageNavigation::MenuView::Seek    ? int(Nav::Seek)
             : pageNavigation::menuView(preferences) == pageNavigation::MenuView::Stats ? int(Nav::Stats)
                                                                                        : -1,
             preferences.value("hide_tab_labels").toBool(false)});
    };
    update();
    syncLinkDiscovery();
    if (choosingTypography && choices->isVisible()) {
        if (choices->isFontPicker()) {
            const int bottom = navigation->contentBottom(size());
            const int pickerHeight = qMin(choices->fontPanelHeight(), qMax(1, bottom - navigation->toolbarHeight()));
            choices->setFontGeometry(QRect(0, bottom - pickerHeight, width(), pickerHeight));
        }
        // Dropdowns cover the reading-panel rules raised by navigation.
        choices->raise();
    }
    if (chromeSheet) {
        const int bottom = navigation->contentBottom(size());
        // At most up to the toolbar, with the sheet's rule on the toolbar's.
        const int room = qMax(1, bottom - navigation->toolbarHeight() + int(m::chrome::heavyRule));
        int sheetHeight = qMin(headerFooterView->preferredHeight(width()), room);
        if (room - sheetHeight < m::headerFooter::toolbarSnap) {
            sheetHeight = room;
        }
        headerFooterView->setGeometry(0, bottom - sheetHeight, width(), sheetHeight);
        headerFooterView->raise();
    }
}

void Reader::showControls() {
    const bool topVisible = topControls->isVisible();
    const bool opening = !topVisible && !bottomControls->isVisible();
    kobalt::ui::PanelTiming timing("menu");
    model->cancelPendingTurn();
    if (opening) {
        // A page turn can defer control updates until after its first paint.
        // Opening the menu needs the completed page's navigation state now.
        syncReadingControls();
        flashAfterMenu = property("reader_dark").toBool();
        if (flashAfterMenu) {
            requestFlash("menu-open");
        }
    }
    dismissLinks();
    linkReturn->hide();
    updateMenuStatus();
    if (orientation)
        orientation->setBlocked(true);
    page->cleanRefresh(false);
    topControls->show();
    bottomControls->show();
    if (!topVisible) {
        topControls->raise();
    }
    if (!navigation) {
        bottomControls->raise();
    }
    updateLinkReturn();
    syncNavigation();
    if (!appearanceEdit.panelDestination()) {
        switch (pageNavigation::menuView(preferences)) {
        case pageNavigation::MenuView::Seek:
            seekOpen = true;
            chapterGraph->hide();
            syncNavigation();
            if (!pageScrubber->selecting())
                pageScrubber->showCurrentPage();
            break;
        case pageNavigation::MenuView::Stats:
            seekOpen = false;
            pageScrubber->cancel();
            openGraph();
            break;
        case pageNavigation::MenuView::None:
            break;
        }
    }
    syncAnnotationControls();
    if (opening) {
        // The display submits disjoint bars separately. Paint their completed
        // frame together so they appear in one screen update.
        setProperty("reader_controls_opening", true);
        update();
        timing.finish(topControls);
    }
}

void Reader::dismissControls() {
    if (pageScrubber)
        pageScrubber->cancel();
    hideRevealedLinks();
    lightPopup->hide();
    chapterGraph->hide();
    if (appearanceEdit.applying())
        return;
    if (general->isVisible() || appearanceEdit.panelOpen()) {
        appearanceEdit.request(AppearanceDestination::Reading);
        if (general->isVisible())
            applySettings();
        else
            finishPreview();
        return;
    }
    choices->hide();
    bookmarkList->hide();
    general->hide();
    menu->hide();
    topControls->hide();
    bottomControls->hide();
    appearanceEdit.discard();
    refresh();
}

void Reader::toggleLight() {
    model->cancelPendingTurn();
    chapterGraph->hide();
    if (lightPopup->isVisible()) {
        lightPopup->hide();
        showControls();
        return;
    }
    appearanceEdit.request(AppearanceDestination::Light);
    choices->hide();
    if (general->isVisible())
        applySettings();
    else if (appearanceEdit.panelOpen())
        finishPreview();
    else {
        menu->hide();
        // Keep Light pending so the controls do not reopen the default view.
        showControls();
        appearanceEdit.clearDestination();
        lightPopup->open();
    }
}

void Reader::showChapters() {
    model->cancelPendingTurn();
    lightPopup->hide();
    chapterGraph->hide();
    QVector<Choice> items;
    QString current;
    for (const auto &chapter : model->chapters()) {
        const int index = items.size();
        const auto name = QString("rrChapter%1").arg(index);
        items.append({chapter.title, chapter.position, name, {}, chapter.depth, chapter.page});
        if (index == model->contentsSelection().current)
            current = name;
    }
    choices->selected = [this](const QString &position) {
        if (model->goTo(position))
            dismissControls();
    };
    choosingTypography = false;
    menu->hide();
    bottomControls->hide();
    // Open at the size navigation gives the list, so the current chapter's page stays in view.
    const int top = navigation ? navigation->statusHeight() : 0;
    choices->setGeometry(0, top, width(), (navigation ? navigation->contentBottom(size()) : height()) - top);
    choices->setChapterPosition(strings::get("stats.page_of").arg(model->page() + 1).arg(model->pages()));
    choices->showChoices(model->title(), std::move(items), current);
    refresh();
}

void Reader::toggleGraph() {
    model->cancelPendingTurn();
    lightPopup->hide();
    if (chapterGraph->isVisible()) {
        chapterGraph->hide();
        showControls();
        refresh();
        return;
    }
    appearanceEdit.request(AppearanceDestination::Stats);
    choices->hide();
    if (general->isVisible())
        applySettings();
    else if (appearanceEdit.panelOpen())
        finishPreview();
    else {
        appearanceEdit.clearDestination();
        openGraph();
    }
}

void Reader::openGraph() {
    model->cancelPendingTurn();
    menu->hide();
    general->hide();
    choices->hide();
    topControls->show();
    bottomControls->hide();
    updateGraph();
    if (navigation)
        chapterGraph->setGeometry(navigation->panelBounds(size(), chapterGraph->height()));
    chapterGraph->open();
    if (navigation)
        navigation->stackPanel(chapterGraph);
    refresh();
}

// The topmost sheet owns page buttons, even when its current page has no arrows.
bool Reader::pageList(int direction) {
    const auto children = findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly);
    for (int index = children.size() - 1; index >= 0; --index) {
        auto *view = children[index];
        if (!view->isVisible())
            continue;
        const auto *modal = dynamic_cast<ui::ModalOverlay *>(view);
        if ((modal && modal->inputBarrier()) || view == choices || view == bookmarkList || view == searchPanel ||
            view == acknowledgments || view == fieldChooser || view == headerFooterView || view == dictionaryPanel ||
            view == linkPanel || view == chapterGraph || view == general || view == menu) {
            ui::turnVisiblePageControl(view, direction);
            return true;
        }
    }
    return false;
}

void Reader::pageButton(int direction) {
    if ((direction != -1 && direction != 1) || !isEnabled() || !isVisible() || lifecycleHidden ||
        services.suspended() || closing || appearanceEdit.applying())
        return;
    const auto *modal = QApplication::activeModalWidget();
    if (modal && modal != this)
        return;
    if (pageList(direction))
        return;
    if (seekOpen && pageScrubber && pageScrubber->isVisible()) {
        pageScrubber->turnPreview(direction);
        return;
    }
    if (!model->canTurn() || appearanceEdit.panelOpen() || general->isVisible() || choices->isVisible() ||
        chapterGraph->isVisible() || revealingLinks || dictionaryPanel->isVisible() || searchPanel->isVisible() ||
        linksActive() || lookupPending || acknowledgments->isVisible() || bookmarkList->isVisible() ||
        lightPopup->isVisible())
        return;
    turnPage(direction);
}

void Reader::turnPage(int direction) {
    if (annotationSaving) {
        return;
    }
    if (pageScrubber)
        pageScrubber->cancel();
    turnStarted.start();
    lastInput.start();
    if (finishDialog || syncDialog || kosyncDialog || annotationErrorDialog || formattingWarningDialog || settingsWriteErrorDialog ||
        !model->canTurn() || closing || exitPending || lifecycleHidden || services.suspended() ||
        book->deviceTransition() || appearanceEdit.panelOpen() || appearanceEdit.applying())
        return;
    if (!annotationMode && !model->busy() && direction > 0 && model->nextPage() == model->page() &&
        book->offerFinish()) {
        showFinishChoice();
        return;
    }
    if (model->turn(direction)) {
        if (annotationMode) {
            ++annotationEpoch;
            annotationPopover->hide();
        }
        ++savedHighlightEpoch;
    }
}

// The layout that bookmark pages belong to, including the page area size.
QString Reader::layoutKey() const {
    return kobalt::bookmarkLayoutKey(model->pages(), model->state(),
                                     QSize(width(), height() - page->topHeight() - page->bottomHeight()));
}

void Reader::syncBookmarks() {
    if (!bookmarks->available() || model->busy() || model->pages() <= 0)
        return;
    const bool resolved = bookmarks->sync();
    page->bookmarked = resolved && bookmarks->marked(model->page());
    pageScrubber->setBookmarkedPages(resolved ? bookmarks->pages() : QVector<int>());
}

void Reader::toggleBookmark() {
    if (bookmarks->toggle(model->page()) != kobalt::BookmarkSession::Toggle::Removed)
        return;
    syncBookmarks();
    page->refreshContent();
    saveSoon();
}

void Reader::requestFlash(const char *reason) {
    setProperty("reader_full_flash", true);
    pagesSinceFlash = 0;
    kobalt::diagnostic("full-flash", {{"reason", reason}}, false);
}

void Reader::recordOpening() {
    if (closing || exportRunning || lifecycleHidden || !isVisible() || model->busy() || model->pages() <= 0 ||
        syncDialog || finishDialog || services.suspended() || book->deviceTransition())
        return;
    recordedOpening = true;
    if (!kosyncStarted) synchronizeKosync();
    // A newer synced position waits for the reader's choice.
    saveNativeProgress();
    save(true);
    scheduleNativeOpeningSave();
}

void Reader::finishExit() {
    if (!exitPending || closing || exportRunning || model->busy() || appearanceEdit.panelOpen() ||
        appearanceEdit.applying() || appearanceEdit.finishPending() || lifecycleHidden)
        return;
    if (finishDialog)
        return;
    if (!services.suspended() && !book->deviceTransition()) {
        book->observeProgress();
        if (book->pendingProgress()) {
            showProgressChoice();
            return;
        }
    }
    if (!finishChoiceResolved && book->offerFinish()) {
        showFinishChoice();
        return;
    }
    exportRunning = true;
    dismissProgressChoice();
    // Capture before freezing paints for the final native writeback.
    exitSleepSnapshot = captureSleepScreen();
    // Keep the last reader frame while asynchronous writeback completes.
    setUpdatesEnabled(false);
    setEnabled(false);
    model->nativePosition([this](const QString &location, const QString &mappingError) {
        QElapsedTimer step;
        step.start();
        save();
        closingTimes.add("checkpoint", step);
        step.restart();
        const bool written = book->writeNative(location, mappingError);
        closingTimes.add("native", step);
        if (!written) {
            exportRunning = false;
            setEnabled(true);
            setUpdatesEnabled(true);
            showProgressChoice();
            return;
        }
        step.restart();
        save(true);
        closingTimes.add("checkpoint", step);
        flushKosync([this] {
            closing = true;
            close();
        });
    });
}

// A page turn does not have to reach storage before the next one. A local
// checkpoint opens the profile database and syncs storage several times, so
// it is queued on the book's writer thread after two quiet seconds (a short
// pause after 30 seconds without a save). Standby does not advance the timer:
// a save still pending when Nickel wants to suspend is queued then
// (flushBeforeStandby), not after the tap that wakes the device.
// The native save must use Nickel's database from this thread, so it never
// runs on page turns. Opening (including retries), sleep, USB, close and power-off write it.
void Reader::saveSoon() {
    if (!localSaveDue)
        unsavedSince.start();
    localSaveDue = true;
    scheduleIdleSave();
}

int Reader::idleSaveDelay() const {
    return localSaveDue && unsavedSince.isValid() && unsavedSince.elapsed() >= 30000 ? 700 : 2000;
}

void Reader::scheduleIdleSave() {
    if (!idleSaveTimer) {
        idleSaveTimer = new QTimer(this);
        idleSaveTimer->setObjectName("rrIdleSaveTimer");
        idleSaveTimer->setSingleShot(true);
        connect(idleSaveTimer, &QTimer::timeout, this, [this] {
            runIdleSave(false);
        });
    }
    if (!localSaveDue)
        return;
    const qint64 delay = idleSaveDelay();
    const qint64 quiet = lastInput.isValid() ? lastInput.elapsed() : delay;
    idleSaveTimer->start(int(qBound(qint64(0), delay - quiet, delay)));
}

void Reader::runIdleSave(bool beforeStandby) {
    if (beforeStandby)
        standbyFlushQueued = false;
    if (!localSaveDue)
        return;
    // These paths save the position themselves.
    if (closing || exportRunning || lifecycleHidden) {
        localSaveDue = false;
        unsavedSince.invalidate();
        return;
    }
    if (model->busy()) {
        if (idleSaveTimer)
            idleSaveTimer->start(250);
        return;
    }
    if (!beforeStandby && lastInput.isValid() && lastInput.elapsed() < idleSaveDelay()) {
        scheduleIdleSave();
        return;
    }
    QElapsedTimer timer;
    timer.start();
    save(false, true);
    kobalt::diagnostic("idle-save", {{"before_standby", beforeStandby}, {"elapsed_ms", double(timer.elapsed())}},
                       false);
}

bool Reader::flushBeforeStandby() {
    // Standby pauses Qt timers. Let the visible confirmation dismiss first.
    auto *notice = dynamic_cast<KosyncNotice *>(kosyncDialog.data());
    if (!lifecycleHidden && notice && notice->completing())
        return false;
    // Let background saves finish their storage syncs before standby.
    if (book->savesPending() || bookmarks->saving() || highlights->saving() ||
        kobalt::storage::profileCheckpointPending())
        return false;
    if (localSaveDue && !closing && !exportRunning && !lifecycleHidden) {
        // Never write from inside Nickel's suspend decision. Queue the save and
        // let Nickel ask again; a busy engine already keeps the device awake.
        if (!std::exchange(standbyFlushQueued, true))
            kobalt::qt::later(0, this, [this] {
                runIdleSave(true);
            });
        return false;
    }
    // Routine position saves do not sync storage. Before standby, sync them
    // once the last sync is a minute old, so a drained battery loses at most
    // about a minute of reading position.
    if (kobalt::storage::profileCheckpointDue()) {
        kobalt::storage::requestProfileCheckpoint();
        return false;
    }
    return true;
}

// `snapshot` marks a close, sleep or shutdown. `background` queues the
// checkpoint on the book's writer thread; a failure shows the same notice.
void Reader::save(bool snapshot, bool background) {
    localSaveDue = false;
    unsavedSince.invalidate();
    if (idleSaveTimer)
        idleSaveTimer->stop();
    QJsonObject state;
    if (!book->checkpoint(&state, background)) {
        page->notice = strings::get("reader.could_not_save_reading_position");
        page->refreshContent();
    }
#ifdef KOBALT_READER_HARDWARE
    // On a device the state snapshot is for inspection over USB. Write it at
    // those moments, not after every reading pause, unless live-state exists.
    if (!snapshot && !liveState)
        return;
#else
    (void)snapshot;
#endif
    if (book->paths().stateReport.isEmpty()) return;
    state["page"] = model->page();
    state["pages"] = model->pages();
    state["cached_pages"] = model->cachedPages();
    state["chapters"] = model->chapters().size();
#ifndef KOBALT_READER_HARDWARE
    QJsonArray links;
    for (const auto &link : model->pageLinks()) {
        QJsonArray rectangles;
        for (const auto &r : link.rectangles)
            rectangles.append(kobalt::qt::array({r.x(), r.y() + page->topHeight(), r.width(), r.height()}));
        links.append(kobalt::qt::object({{"source", link.source}, {"rectangles", rectangles}}));
    }
    state["links"] = links;
    QJsonArray counts, positions, titles, pages;
    for (const auto &chapter : model->chapterGraph()) {
        counts.append(double(chapter.words));
        positions.append(chapter.position);
        titles.append(chapter.title);
        pages.append(chapter.page);
    }
    state["word_counts"] = kobalt::qt::object({{"ready", model->totalWords() >= 0},
                                               {"total_words", double(model->totalWords())},
                                               {"words", counts},
                                               {"positions", positions},
                                               {"titles", titles},
                                               {"mapped_pages", pages},
                                               {"current", model->graphSelection().current},
                                               {"next", model->graphSelection().next}});
    const auto estimates = book->estimates();
    state["reading_estimates"] = kobalt::qt::object({{"chapter_ms", double(estimates.chapterMilliseconds)},
                                                     {"book_ms", double(estimates.bookMilliseconds)},
                                                     {"learning", estimates.learningPace}});

#endif
    state["resource_cache"] = model->resourceStats();
    state["busy"] = model->busy();
    state["page_text_only"] = model->image().text("kobalt.animation_text_only") == "true";
    state["page_quality_mono"] = !kobalt::pageRefreshRegions(model->image(), false).isEmpty();
    state["page_quality_colour"] = !kobalt::pageRefreshRegions(model->image(), true).isEmpty();
    state["native_ui_font"] = services.uiFont().family();
    state["native_ui_font_resolved"] = QFontInfo(services.uiFont()).family();
    state["ui_font_alias"] = font().family();
    state["ui_font_substitute"] = kobalt::ui::interfaceFontFamily();
    state["ui_font_resolved"] = QFontInfo(font()).family();
    state["interface_weight"] = kobalt::ui::interfaceWeight();
    state["reading_chrome_font"] = QFontInfo(page->font()).family();
    state["session_reading_ms"] = double(book->readingMilliseconds());
    state["menu_session_text"] = menuSession->text();
    state["menu_clock_font"] = QFontInfo(menuClock->font()).family();
    state["menu_battery_font"] = QFontInfo(menuBattery->font()).family();
    state["ui_icon_set"] = QStringLiteral("Lucide");
    state["typography_button_font"] = QFontInfo(findChild<QPushButton *>("rrTypography")->font()).family();
    state["benchmark_typography"] = model->temporaryTypography();
    state["display_settings"] = preferences;
    state["header_text_pixels"] = page->headerTextPixels();
    state["footer_text_pixels"] = page->footerTextPixels();
    state["header_footer_preset"] = kobalt::headerfooter::presetValue(chromePreset);
    state["reading_top"] = page->topHeight();
    state["reading_bottom"] = page->bottomHeight();
    state["reading_image_height"] = model->image().height();
    state["preview_image_height"] = appearanceEdit.preview() ? appearanceEdit.preview()->image().height() : 0;
    state["screen_orientation"] = int(QGuiApplication::primaryScreen()->orientation());
    state["auto_rotation_available"] = orientation && orientation->automaticAvailable();
    state["viewport_width"] = width();
    state["viewport_height"] = height();
    state["header_text"] = page->headerText();
    state["contents_page"] = choices->chapterPage();
    state["font_samples"] = choices->fontSamples();
    state["native_font_families"] = QJsonArray::fromStringList(pickerNativeFamilies);
    state["font_button_family"] = fontButton->resolvedFace();
    state["graph_page"] = chapterGraph->graphPage();
    state["graph_pages"] = chapterGraph->graphPages();
    state["active_panel"] =
        QString::fromLatin1(chapterGraph->isVisible()    ? "chapters"
                            : general->isVisible()       ? "settings"
                            : appearanceEdit.panelOpen() ? panels->currentIndex() == 0 ? "typography" : "layout"
                                                         : "");
    state["appearance_panel_open"] = appearanceEdit.panelOpen();
    state["preview_ready"] = appearanceEdit.ready();
    state["preview_busy"] = appearanceEdit.preview() && appearanceEdit.preview()->busy();
    state["preview_settings"] = appearanceEdit.ready() ? appearanceEdit.preview()->state() : QJsonObject();
    state["typography_panel"] = menu->isVisible();
    state["annotation_dragging"] = annotationDragging;
    state["bookmarked"] = page->bookmarked;
    state["bookmark_count"] = bookmarks->available() ? int(bookmarks->bookmarks().size()) : -1;
    state["bookmark_pages"] = [this] {
        QJsonArray pages;
        for (int p : bookmarks->pages())
            pages.append(p);
        return pages;
    }();
    writeJson(book->paths().stateReport, state);
}

}

namespace kobalt {
ReaderWindow *createReaderWindow(std::unique_ptr<ReadingBook> book, ReaderOrientation *orientation,
                                 ReaderHostServices services) {
    return new reader::Reader(std::move(book), orientation, std::move(services));
}
}

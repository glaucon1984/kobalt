#pragma once
#include <QImage>
#include <functional>

namespace kobalt::pocketbook {
// The PocketBook firmware draws its standby (lock) screen from
// system/resources/<theme>/taskmgr_lock_background.bmp (+ _landscape) when those files exist,
// from a copy cached when the app was launched. Kobalt's sleep image is written there and the
// firmware's task manager is asked to reload its copy; that request must not be sent while the
// device is going to sleep (it then stays half asleep), so it is deferred to the next wake.
// The task is pinned to portrait and the firmware draws the lock bitmap unrotated, whatever the
// device's physical orientation, so the image is turned into the panel's own orientation (the
// pbink plugin's current rotation) and written to the portrait file only. 8-bit greyscale BMP at
// panel size. Returns false when nothing was written.
// The same bitmap also goes to system/logo/bookcover, the image the firmware shows at power-off
// when its Power-off logo setting is the book cover (read at shutdown; the KOReader cover
// plugins write the same file). It is copied from the lock file after the first write of a
// book and then at most every 30 s, and always at standby, so a power-off from the lock screen
// shows the page or cover the sleep screen showed.
bool writeLockImages(const QImage &image);
// Copies the lock bitmap on disk to the power-off file now (blocking, tens of ms; waits for a
// lock image write in progress). False when the power-off file is already that bitmap.
bool writePowerOffImage();
// Stores a capture of the panel (pbinkPanelImage, the page as it was shown) as the boot logo
// through the firmware's own tool (iv2sh WriteStartupLogo, which writes the bootloader's flash
// partition; the firmware does the same for its reader when its Boot logo setting is the book
// cover). The bitmap is written on a worker thread (24-bit, as KOReader's) next to the binary,
// with the book's path beside it, then the tool runs as a background process: the caller keeps
// the device awake until `done` runs on the GUI thread. Returns false, without calling `done`,
// when a write is running or the image is null.
bool writeBootLogo(const QImage &panelImage, const QString &bookPath, const char *reason,
                   std::function<void()> done = {});
bool bootLogoWriteInFlight();
// The last boot logo image, when it was captured for this book and the book has not been closed
// since (markCurrentBook with an empty path forgets it); null otherwise. Shown while the book
// loads, so a reopen after a power-off goes boot logo -> same page -> live page.
QImage savedBootLogo(const QString &bookPath);
// Tells the firmware which book this task has open (inkview's /tmp/.current marker and the
// subtask info, as KOReader does), so that a power-off saves it and the next boot reopens it
// with Kobalt when the firmware's "open last book" setting is on. An empty path clears it.
void markCurrentBook(const QString &bookPath, const QString &title = {});
// The same on a worker thread (a write takes 0.2-0.4 s on the device). Returns false, without
// calling `done`, for an image identical to the last one written at the same panel rotation; otherwise `done(ok)` runs on
// the GUI thread afterwards.
bool writeLockImagesAsync(const QImage &image, std::function<void(bool)> done);
// Asks taskmgr.app to reload the lock image now (background shell command, as the KOReader
// standbycover plugin does). `done` runs on the GUI thread once the request has returned plus a
// margin for taskmgr's own work. A lock (standby) that lands while taskmgr reloads leaves the
// device half asleep, so the caller keeps the device awake until then: the firmware refuses a
// lock while iv_sleepmode(0) holds.
void requestLockImageReload(const char *reason, std::function<void()> done = {});
// True from a reload request until its `done` ran.
bool lockImageReloadInFlight();
// A write made while the device goes to sleep defers its reload request to the next wake.
void markLockReloadPending();
bool takeLockReloadPending();
}

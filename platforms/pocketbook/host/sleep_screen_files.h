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
// A power-off image goes to system/logo/bookcover, the image the firmware shows at power-off
// when its Power-off logo setting is the book cover (read at shutdown; the KOReader cover
// plugins write the same file). It is the same page or cover under the "Powered off" status
// (as on Kobo, where the power-off screen tells a switched-off device from a sleeping one), so
// `powerOff` is the reader's power-off composition for the same backdrop; a null one falls back
// to a copy of the lock bitmap. It is written after the first lock image of a book and then at
// most every 30 s while reading, and always at standby (writePowerOffImage), so a power-off from
// the lock screen shows the page the sleep screen showed, marked as powered off.
bool writeLockImages(const QImage &image, const QImage &powerOff = {});
// Writes the power-off file now (blocking, tens of ms; waits for a lock image write in
// progress): `image` when given (the reader's power-off composition for the sleep image just
// presented), else the power-off image pending from the last page turn, else a copy of the lock
// bitmap. False when the file is already current or nothing is there to write.
bool writePowerOffImage(const QImage &image = {});
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
// The last boot logo image, when it was captured for this book, the book has not been closed
// since (markCurrentBook with an empty path forgets it) and the device has been powered off
// since (the capture records the kernel's boot id); null otherwise. Returned as logical
// content (the capture's own turn undone), to be painted on the screen as it is rotated now.
// Shown while the book loads, so a reopen after a power-off goes boot logo -> same page -> live page.
QImage savedBootLogo(const QString &bookPath);
// The capture no longer stands for a session to resume (the book was closed, or a PC took the
// storage): the next open of the book gets the opening view.
void forgetBootLogo();
// Tells the firmware which book this task has open (inkview's /tmp/.current marker and the
// subtask info, as KOReader does), so that a power-off saves it and the next boot reopens it
// with Kobalt when the firmware's "open last book" setting is on. An empty path clears it.
void markCurrentBook(const QString &bookPath, const QString &title = {});
// The same on a worker thread (a write takes 0.2-0.4 s on the device). Returns false, without
// calling `done`, for an image identical to the last one written at the same panel rotation
// (the power-off image follows the same inputs, so it is current then too); otherwise `done(ok)`
// runs on the GUI thread afterwards.
bool writeLockImagesAsync(const QImage &image, const QImage &powerOff, std::function<void(bool)> done);
// Asks taskmgr.app to reload the lock image now (background shell command, as the KOReader
// standbycover plugin does). `done` runs on the GUI thread once the request has returned plus a
// margin for taskmgr's own work. A lock (standby) that lands while taskmgr reloads leaves the
// device half asleep, so the caller keeps the device awake until then: the firmware refuses a
// lock while iv_sleepmode(0) holds.
void requestLockImageReload(const char *reason, std::function<void()> done = {});
// True from a reload request until its `done` ran.
bool lockImageReloadInFlight();
// The firmware's own lock screen again (the built-in sleep screen chosen): removes Kobalt's lock
// bitmaps (the portrait file and a landscape one another app may have left), after which the
// firmware draws its built-in picture; the caller asks taskmgr to reload. Returns false when
// there was nothing to remove. The next Kobalt image is written whatever was written last.
bool restoreFirmwareLockImages();
// The book's plain cover as the power-off file, what the firmware's own reader writes there
// (the built-in sleep screen chosen): fitted to the panel, turned as the device is held, no
// status. Blocking, tens of ms. False for a null cover or a failed write.
bool writeCoverAsPowerOffImage(const QImage &cover);
// At the book's close, and at a USB plug (PC Link ends the app without a close), in every sleep
// screen mode: the firmware's own screens from here on. The lock bitmaps go (the firmware draws
// its picture; taskmgr is told, detached), the power-off file and the boot logo get the book's
// plain cover as the firmware's reader leaves them (the firmware's logo settings decide whether
// they show), and the capture kept for the boot is dropped. Blocking for the bitmap writes
// (under a second); the firmware's tool runs on after the process has quit. `when` names the
// occasion in the log.
void leaveFirmwareScreens(const QImage &cover, const char *when);
// A write made while the device goes to sleep defers its reload request to the next wake.
void markLockReloadPending();
bool takeLockReloadPending();
}

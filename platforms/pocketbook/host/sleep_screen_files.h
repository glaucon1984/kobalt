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
bool writeLockImages(const QImage &image);
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

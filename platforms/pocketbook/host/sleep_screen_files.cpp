#include "sleep_screen_files.h"
#include "app/reader/views/sleep_screen.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QTransform>
#include <qpa/qplatformnativeinterface.h>
#include <QProcess>
#include <QSaveFile>
#include <QTimer>
#include <QScreen>
#include <QVector>
#include <QCoreApplication>
#include <inkview.h>
#include <mutex>
#include <thread>

namespace kobalt::pocketbook {
namespace {
bool pendingReload = false;
int reloadsInFlight = 0;
const int reloadMarginMs = 1000;
std::mutex writeMutex;
QImage lastWritten; // GUI thread only
int lastRotation = -1; // panel rotation lastWritten was turned by
const char *lineDir = "/mnt/ext1/system/resources/Line";
const char *lockPortrait = "/mnt/ext1/system/resources/Line/taskmgr_lock_background.bmp";
// What the firmware shows at power-off while Settings > Personalize > Logos > Power-off logo is
// the book cover (offlogo=@cover_logo); read at shutdown, not cached. Same 8-bit BMP format.
const char *powerOffCover = "/mnt/ext1/system/logo/bookcover";
QElapsedTimer lastPowerOffCopy; // under writeMutex
bool powerOffStale = false; // a newer power-off picture than the file's; under writeMutex
// The power-off composition for the lock bitmap on disk, turned to the panel, 8-bit (2 MB at
// panel size); null when the reader gave none (the lock bitmap is copied then). Under writeMutex.
QImage pendingPowerOff;
const int powerOffCopyIntervalMs = 30000;
bool bootLogoInFlight = false; // GUI thread only
const int bootLogoTimeoutMs = 60000;
// inkview's CURRENTBOOK: the book open in the foreground task, which the firmware saves at
// power-off and reopens at boot when its "open last book" setting is on (as KOReader does).
const char *currentBookMarker = "/tmp/.current";

QString bootLogoPath() {
    return QCoreApplication::applicationDirPath() + QStringLiteral("/bootlogo.bmp");
}
QString bootLogoBookPath() {
    return QCoreApplication::applicationDirPath() + QStringLiteral("/bootlogo.book");
}

// The kernel's id for this boot: a new one at every power-on, the same through standby. Empty
// when unreadable.
QByteArray bootId() {
    QFile file(QStringLiteral("/proc/sys/kernel/random/boot_id"));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readLine().trimmed();
}

bool writeBmp24(const QImage &image, const QString &path) {
    // What KOReader hands the firmware's logo tool; the 8-bit form is untested there.
    const QImage rgb = image.convertToFormat(QImage::Format_RGB888);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || !rgb.save(&file, "BMP"))
        return false;
    return file.commit();
}

QVector<QRgb> greyTable() {
    QVector<QRgb> table(256);
    for (int i = 0; i < 256; ++i)
        table[i] = qRgb(i, i, i);
    return table;
}

bool writeBmp8(const QImage &image, const QString &path) {
    // 8-bit indexed BMP with a grey palette, the format the firmware's own images use.
    // Grayscale8 and Indexed8 share the byte layout: wrap the grey bytes instead of letting
    // Qt search the palette for every pixel.
    static const QVector<QRgb> table = greyTable();
    const QImage grey = image.convertToFormat(QImage::Format_Grayscale8);
    QImage indexed(grey.constBits(), grey.width(), grey.height(), grey.bytesPerLine(), QImage::Format_Indexed8);
    indexed.setColorTable(table);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    if (!indexed.save(&file, "BMP"))
        return false;
    return file.commit();
}

void backupOnce(const QString &path) {
    if (QFile::exists(path) && !QFile::exists(path + ".orig"))
        QFile::copy(path, path + ".orig");
}

QSize panelSize() {
    // The physical panel is portrait: the smaller side is the width whatever the rotation.
    const QSize logical = QGuiApplication::primaryScreen()->geometry().size();
    return logical.width() <= logical.height() ? logical : logical.transposed();
}

// Degrees the pbink plugin turns logical content clockwise to reach the panel (0/90/180/270).
int panelRotation() {
    auto *native = QGuiApplication::platformNativeInterface();
    if (auto fn = native ? native->platformFunction("pbinkRotation") : nullptr)
        return reinterpret_cast<int (*)()>(fn)();
    return 0;
}

// The sleep or power-off image, composed in the reader's logical orientation, as the panel
// shows it: turned the way the plugin turns every frame (so the lock screen reads the way the
// device is held) and fitted to the portrait panel.
QImage toPanel(const QImage &image, int rotation) {
    const QSize panel = panelSize();
    QImage turned = rotation ? image.transformed(QTransform().rotate(rotation)) : image;
    return turned.size() == panel ? turned : reader::sleepCoverBackground(turned, panel);
}

void powerOffWritten() {
    lastPowerOffCopy.start();
    powerOffStale = false;
}

// Copies the lock bitmap on disk to the power-off file (atomically: a shutdown in the middle
// must not leave a torn file). Caller holds writeMutex.
bool copyLockToPowerOffLocked() {
    QFile lock(lockPortrait);
    if (!lock.open(QIODevice::ReadOnly))
        return false;
    const QByteArray bytes = lock.readAll();
    lock.close();
    if (bytes.isEmpty())
        return false;
    backupOnce(powerOffCover);
    QSaveFile file(powerOffCover);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        return false;
    const bool ok = file.commit();
    if (ok)
        powerOffWritten();
    return ok;
}

// Writes the pending power-off composition, or without one a copy of the lock bitmap (the
// sleep screen's picture, status and all). Caller holds writeMutex.
bool flushPowerOffLocked() {
    if (pendingPowerOff.isNull())
        return copyLockToPowerOffLocked();
    backupOnce(powerOffCover);
    const bool ok = writeBmp8(pendingPowerOff, powerOffCover);
    if (ok)
        powerOffWritten();
    return ok;
}

// `paced`: also write the power-off file when its pace allows (the page-turn path); the standby
// path writes it right afterwards with a fresh composition (writePowerOffImage), so it skips this.
bool writeLockImagesNow(const QImage &image, const QImage &powerOff, int rotation, bool paced) {
    std::lock_guard<std::mutex> guard(writeMutex);
    QElapsedTimer clock;
    clock.start();
    QDir().mkpath(lineDir);
    backupOnce(lockPortrait);
    const bool ok = writeBmp8(toPanel(image, rotation), lockPortrait);
    qInfo("kobalt-pb: lock image %s (rotation %d) in %lld ms", ok ? "written" : "NOT written", rotation,
          static_cast<long long>(clock.elapsed()));
    if (!ok)
        return false;
    // The power-off file follows at a slower pace (the first write of the book, then at most
    // every 30 s while reading); a standby writes whatever is still pending (writePowerOffImage).
    // Kept 8-bit: the bitmap's own form, a quarter of the composed image.
    pendingPowerOff = powerOff.isNull() ? QImage() : toPanel(powerOff, rotation).convertToFormat(QImage::Format_Grayscale8);
    powerOffStale = true;
    if (paced && (!lastPowerOffCopy.isValid() || lastPowerOffCopy.elapsed() >= powerOffCopyIntervalMs)) {
        const bool written = flushPowerOffLocked();
        qInfo("kobalt-pb: power-off image %s", written ? "written" : "NOT written");
    }
    return true;
}
}

bool writePowerOffImage(const QImage &image) {
    const int rotation = image.isNull() ? 0 : panelRotation();
    std::lock_guard<std::mutex> guard(writeMutex);
    if (!image.isNull()) {
        pendingPowerOff = toPanel(image, rotation).convertToFormat(QImage::Format_Grayscale8);
        powerOffStale = true;
    }
    if (!powerOffStale || (pendingPowerOff.isNull() && !QFile::exists(lockPortrait)))
        return false;
    QElapsedTimer clock;
    clock.start();
    const bool ok = flushPowerOffLocked();
    qInfo("kobalt-pb: power-off image %s (standby%s) in %lld ms", ok ? "written" : "NOT written",
          image.isNull() ? ", pending" : "", static_cast<long long>(clock.elapsed()));
    return ok;
}

bool writeLockImages(const QImage &image, const QImage &powerOff) {
    if (image.isNull())
        return false;
    const int rotation = panelRotation();
    const bool ok = writeLockImagesNow(image, powerOff, rotation, false);
    if (ok) {
        lastWritten = image;
        lastRotation = rotation;
    }
    return ok;
}

bool writeLockImagesAsync(const QImage &image, const QImage &powerOff, std::function<void(bool)> done) {
    // Same image, same rotation: the bitmap on disk is current. The rotation is part of the
    // identity: a page reads the same upside down (same logical size), and the turn alone used
    // to be skipped, leaving the firmware the previous orientation's bitmap until the wake
    // reload (seen: inverted lock screen at the first standby after turning the device).
    const int rotation = panelRotation();
    if (image.isNull() || (image == lastWritten && rotation == lastRotation))
        return false;
    lastWritten = image;
    lastRotation = rotation;
    std::thread([image, powerOff, rotation, done = std::move(done)] {
        const bool ok = writeLockImagesNow(image, powerOff, rotation, true);
        QMetaObject::invokeMethod(QCoreApplication::instance(), [ok, done] { done(ok); }, Qt::QueuedConnection);
    }).detach();
    return true;
}

namespace {
void runBootLogoTool(const QString &why, std::function<void()> done) {
    auto *process = new QProcess(QCoreApplication::instance());
    auto *clock = new QElapsedTimer;
    clock->start();
    QObject::connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), process,
                     [process, clock, why, done](int code, QProcess::ExitStatus) {
                         qInfo("kobalt-pb: boot logo write (%s) done in %lld ms, exit %d", qPrintable(why),
                               static_cast<long long>(clock->elapsed()), code);
                         delete clock;
                         process->deleteLater();
                         bootLogoInFlight = false;
                         if (done)
                             done();
                     });
    QTimer::singleShot(bootLogoTimeoutMs, process, [process] {
        if (process->state() != QProcess::NotRunning) {
            qWarning("kobalt-pb: boot logo write still running after %d ms, killing it", bootLogoTimeoutMs);
            process->kill();
        }
    });
    process->start(QStringLiteral("/ebrmain/bin/iv2sh"), {QStringLiteral("WriteStartupLogo"), bootLogoPath()});
}
}

bool writeBootLogo(const QImage &panelImage, const QString &bookPath, const char *reason,
                   std::function<void()> done) {
    // The bootloader shows a logo kept in a flash partition; `iv2sh WriteStartupLogo <bmp>`
    // stores one there (KOReader's Cover image plugin does the same; the firmware itself does it
    // for its own reader when Settings > Personalize > Logos > Boot logo is the book cover, which
    // is also what makes the setting show a book cover at all). The image is the panel as it
    // was: a device that powers itself off from standby boots straight into the page that was
    // being read, and the reopen (see markCurrentBook) shows the same page while the book
    // loads. Slow (seconds): a background process, with the device kept awake by the caller;
    // skipped while one runs.
    if (bootLogoInFlight || panelImage.isNull())
        return false;
    bootLogoInFlight = true;
    const QString why = QString::fromLatin1(reason);
    // The capture is the panel as turned for the holder at the time; the firmware draws the boot
    // logo as it is, so that is the right file. The turn is kept beside it, so that the page can
    // be shown again in whatever orientation the device is held at the next start, and the boot
    // id with it, so that the capture is only ever shown after a power-off (savedBootLogo).
    const int rotation = panelRotation();
    const QByteArray boot = bootId();
    if (boot.isEmpty())
        qWarning("kobalt-pb: boot id unreadable: the boot capture cannot be tied to this boot");
    std::thread([panelImage, bookPath, rotation, boot, why, done = std::move(done)] {
        QElapsedTimer clock;
        clock.start();
        const bool ok = writeBmp24(panelImage, bootLogoPath());
        if (ok) {
            QSaveFile book(bootLogoBookPath());
            if (book.open(QIODevice::WriteOnly)) {
                book.write(bookPath.toUtf8() + '\n' + QByteArray::number(rotation) + '\n' + boot + '\n');
                book.commit();
            }
        }
        qInfo("kobalt-pb: boot logo image %s (%s) in %lld ms", ok ? "written" : "NOT written", qPrintable(why),
              static_cast<long long>(clock.elapsed()));
        QMetaObject::invokeMethod(QCoreApplication::instance(), [ok, why, done] {
            if (!ok) {
                bootLogoInFlight = false;
                if (done)
                    done();
                return;
            }
            runBootLogoTool(why, done);
            qInfo("kobalt-pb: boot logo write started (%s)", qPrintable(why));
        }, Qt::QueuedConnection);
    }).detach();
    return true;
}

QImage savedBootLogo(const QString &bookPath) {
    QFile book(bootLogoBookPath());
    if (!book.open(QIODevice::ReadOnly))
        return {};
    const QList<QByteArray> lines = book.readAll().split('\n');
    if (lines.isEmpty() || QString::fromUtf8(lines[0]) != bookPath)
        return {};
    // The capture stands for a session a power-off ended. Taken in this very boot, it is a
    // session that ended some other way (PC Link took the storage, a crash), and the open is an
    // ordinary one with the opening view. The boot id is the only sure sign of a power-off in
    // between: the firmware's background event looks the same for a standby and for a PC plug,
    // and its USB state can lag that event (a plug was taken for a standby once).
    const QByteArray boot = lines.size() > 2 ? lines[2].trimmed() : QByteArray();
    if (!boot.isEmpty() && boot == bootId()) {
        qInfo("kobalt-pb: boot capture is from this boot (no power-off since): opening view");
        return {};
    }
    const int rotation = lines.size() > 1 ? lines[1].toInt() : 0;
    // Back to the page as logical content; the screen's current rotation turns it for the holder.
    const QImage panel(bootLogoPath());
    return rotation ? panel.transformed(QTransform().rotate(-rotation)) : panel;
}

void markCurrentBook(const QString &bookPath, const QString &title) {
    // What KOReader's notifyBookState does: the marker file plus the task's subtask info, which
    // is what the task manager and the "current book" logic read. Without them the firmware
    // forgot the book at power-off and booted to its home screen instead of reopening it.
    QFile::remove(QString::fromLatin1(currentBookMarker));
    if (!bookPath.isEmpty()) {
        QFile marker(QString::fromLatin1(currentBookMarker));
        if (marker.open(QIODevice::WriteOnly)) {
            marker.write(bookPath.toUtf8());
            marker.close();
        }
    } else {
        // The book was closed on purpose: the next open of it is a fresh start with the opening
        // view, not a resume from the boot capture. (A power-off leaves the capture valid.)
        forgetBootLogo();
    }
    const QByteArray name = (bookPath.isEmpty() ? QStringLiteral("kobalt") : title + QStringLiteral(" - kobalt")).toUtf8();
    const QByteArray book = (bookPath.isEmpty() ? QStringLiteral("N/A") : bookPath).toUtf8();
    SetSubtaskInfo(GetCurrentTask(), 0, name.constData(), book.constData());
    qInfo("kobalt-pb: current book %s", bookPath.isEmpty() ? "cleared" : "marked for the task manager");
}

bool bootLogoWriteInFlight() {
    return bootLogoInFlight;
}

void forgetBootLogo() {
    QFile::remove(bootLogoBookPath());
}

bool restoreFirmwareLockImages() {
    bool removed = false;
    for (const char *path : {lockPortrait, "/mnt/ext1/system/resources/Line/taskmgr_lock_background_landscape.bmp"}) {
        if (!QFile::exists(QString::fromLatin1(path)))
            continue;
        if (QFile::remove(QString::fromLatin1(path))) {
            removed = true;
            qInfo("kobalt-pb: removed %s (built-in sleep screen)", path);
        } else {
            qWarning("kobalt-pb: cannot remove %s", path);
        }
    }
    lastWritten = QImage();
    lastRotation = -1;
    return removed;
}

bool writeCoverAsPowerOffImage(const QImage &cover) {
    if (cover.isNull())
        return false;
    const int rotation = panelRotation();
    std::lock_guard<std::mutex> guard(writeMutex);
    QElapsedTimer clock;
    clock.start();
    backupOnce(powerOffCover);
    const bool ok = writeBmp8(toPanel(cover, rotation), powerOffCover);
    if (ok) {
        pendingPowerOff = QImage();
        powerOffWritten();
    }
    qInfo("kobalt-pb: power-off image %s (plain cover, rotation %d) in %lld ms", ok ? "written" : "NOT written",
          rotation, static_cast<long long>(clock.elapsed()));
    return ok;
}

void markLockReloadPending() {
    pendingReload = true;
}
bool takeLockReloadPending() {
    const bool pending = pendingReload;
    pendingReload = false;
    return pending;
}

bool lockImageReloadInFlight() {
    return reloadsInFlight > 0;
}

// taskmgr.app caches the lock image; EVT_CONFIGCHANGED (154) makes it reload. Found by apastuszak
// (coverprogresspocketbook.koplugin); same command as standbycover.koplugin, without its
// `sync`: taskmgr reads the file through the page cache, and a lock press that arrives while
// it reloads is dropped by the firmware, so the reload must be as short as possible.
static QString lockReloadCommand() {
    return QStringLiteral(
        "p=$(ps | grep '[t]askmgr\\.app' | grep -v visualizer | awk '{print $1}' | head -n 1); "
        "[ -n \"$p\" ] && exec /ebrmain/bin/iv2sh SendEventTo \"$p\" 154 0");
}

void leaveFirmwareScreens(const QImage &cover, const char *when, bool filesOnly) {
    QElapsedTimer clock;
    clock.start();
    // The lock screen: the firmware's own picture. The reload outlives this process.
    if (restoreFirmwareLockImages() && !filesOnly) {
        QProcess::startDetached(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), lockReloadCommand()});
        qInfo("kobalt-pb: lock image reload requested (%s, detached)", when);
    }
    forgetBootLogo();
    if (cover.isNull()) {
        qInfo("kobalt-pb: firmware screens (%s): no cover, power-off and boot logos left as written", when);
        return;
    }
    if (filesOnly) {
        writeCoverAsPowerOffImage(cover);
        qInfo("kobalt-pb: firmware screens (%s): files only, in %lld ms", when, static_cast<long long>(clock.elapsed()));
        return;
    }
    // The power-off file and the boot logo: the book's plain cover, what the firmware's reader
    // leaves in both when its logo settings say "book cover". The boot logo goes through the
    // firmware's tool, started detached so it finishes after this process has quit (under a
    // second); skipped while a capture is still being written, two writers on the partition
    // being worse than a stale page.
    writeCoverAsPowerOffImage(cover);
    if (bootLogoInFlight) {
        qWarning("kobalt-pb: boot logo busy (%s): the last capture stays as the boot logo", when);
    } else if (writeBmp24(toPanel(cover, panelRotation()), bootLogoPath())) {
        QProcess::startDetached(QStringLiteral("/ebrmain/bin/iv2sh"), {QStringLiteral("WriteStartupLogo"), bootLogoPath()});
        qInfo("kobalt-pb: boot logo write started (%s, cover, detached)", when);
    } else {
        qWarning("kobalt-pb: boot logo image NOT written (%s)", when);
    }
    qInfo("kobalt-pb: firmware screens restored (%s) in %lld ms", when, static_cast<long long>(clock.elapsed()));
}

void requestLockImageReload(const char *reason, std::function<void()> done) {
    const QString command = lockReloadCommand();
    auto *process = new QProcess(QCoreApplication::instance());
    auto *clock = new QElapsedTimer;
    clock->start();
    const QString why = QString::fromLatin1(reason);
    ++reloadsInFlight;
    QObject::connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), process,
                     [process, clock, why, done](int code, QProcess::ExitStatus) {
                         qInfo("kobalt-pb: lock image reload (%s) done in %lld ms, exit %d", qPrintable(why),
                               static_cast<long long>(clock->elapsed()), code);
                         delete clock;
                         process->deleteLater();
                         QTimer::singleShot(reloadMarginMs, QCoreApplication::instance(), [done] {
                             --reloadsInFlight;
                             if (done)
                                 done();
                         });
                     });
    process->start("/bin/sh", {"-c", command});
    qInfo("kobalt-pb: lock image reload requested (%s)", reason);
}
}

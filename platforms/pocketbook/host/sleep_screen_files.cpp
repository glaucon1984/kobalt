#include "sleep_screen_files.h"
#include "interfaces/reader/views/sleep_screen.h"
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

bool writeLockImagesNow(const QImage &image, int rotation) {
    std::lock_guard<std::mutex> guard(writeMutex);
    QElapsedTimer clock;
    clock.start();
    QDir().mkpath(lineDir);
    backupOnce(lockPortrait);
    const QSize panel = panelSize();
    // The sleep image is composed in the reader's logical orientation; turn it the way the
    // plugin turns every frame, so the lock screen reads the way the device is held.
    QImage turned = rotation ? image.transformed(QTransform().rotate(rotation)) : image;
    const QImage portrait = turned.size() == panel ? turned : reader::sleepCoverBackground(turned, panel);
    const bool ok = writeBmp8(portrait, lockPortrait);
    qInfo("kobalt-pb: lock image %s (rotation %d) in %lld ms", ok ? "written" : "NOT written", rotation,
          static_cast<long long>(clock.elapsed()));
    return ok;
}
}

bool writeLockImages(const QImage &image) {
    if (image.isNull())
        return false;
    const int rotation = panelRotation();
    const bool ok = writeLockImagesNow(image, rotation);
    if (ok) {
        lastWritten = image;
        lastRotation = rotation;
    }
    return ok;
}

bool writeLockImagesAsync(const QImage &image, std::function<void(bool)> done) {
    // Same image, same rotation: the bitmap on disk is current. The rotation is part of the
    // identity: a page reads the same upside down (same logical size), and the turn alone used
    // to be skipped, leaving the firmware the previous orientation's bitmap until the wake
    // reload (seen: inverted lock screen at the first standby after turning the device).
    const int rotation = panelRotation();
    if (image.isNull() || (image == lastWritten && rotation == lastRotation))
        return false;
    lastWritten = image;
    lastRotation = rotation;
    std::thread([image, rotation, done = std::move(done)] {
        const bool ok = writeLockImagesNow(image, rotation);
        QMetaObject::invokeMethod(QCoreApplication::instance(), [ok, done] { done(ok); }, Qt::QueuedConnection);
    }).detach();
    return true;
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

void requestLockImageReload(const char *reason, std::function<void()> done) {
    // taskmgr.app caches the image; EVT_CONFIGCHANGED (154) makes it reload. Found by apastuszak
    // (coverprogresspocketbook.koplugin); same command as standbycover.koplugin, without its
    // `sync`: taskmgr reads the file through the page cache, and a lock press that arrives while
    // it reloads is dropped by the firmware, so the reload must be as short as possible.
    const QString command = QStringLiteral(
        "p=$(ps | grep '[t]askmgr\.app' | grep -v visualizer | awk '{print $1}' | head -n 1); "
        "[ -n \"$p\" ] && exec /ebrmain/bin/iv2sh SendEventTo \"$p\" 154 0");
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

// Kobalt on PocketBook (spike): opens the EPUB given on the command line with Kobalt's reader,
// drawn through inkview by the statically linked pbink platform plugin.
#include "host.h"
#include "firmware_power.h"
#include "assets/assets.h"
#include "assets/bundled_fonts.h"
#include "dictionaries/manager.h"
#include "ui/style/fonts.h"
#include "ui/style/scale.h"
#include "reading/reading_density.h"
#include "support/diagnostics.h"
#include "platform/posix/diagnostics.h"
#include <QApplication>
#include <unistd.h>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFontDatabase>
#include <QMutex>
#include <QThread>
#include <QTime>
#include <QWaitCondition>
#include <cerrno>
#include <QScreen>
#include <QSslSocket>
#include <QSslConfiguration>
#include <QTimer>
#include <qpa/qplatformnativeinterface.h>
#include <QtPlugin>
#include <cstdio>

Q_IMPORT_PLUGIN(PbInkIntegrationPlugin)
Q_IMPORT_PLUGIN(QJpegPlugin)
Q_IMPORT_PLUGIN(QGifPlugin)

namespace {
// Offline dictionary install: Kobalt downloads its StarDict dictionaries over HTTPS, which this
// build has no TLS for. A catalogue zip (same file, downloaded on a computer) dropped into
// applications/kobalt/dictionaries/<id>.zip is installed from here on the next start.
// The PocketBook library indexes every *.txt it finds as a book: drop the README.txt that
// Kobalt's dictionary installer keeps (optional for lookups).
void removeDictionaryReadmes(const QString &dictionaryRoot) {
    for (const auto &entry : QDir(dictionaryRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        QFile::remove(dictionaryRoot + "/" + entry + "/README.txt");
}
void importDictionaries(const QString &dropFolder, const QString &dictionaryRoot) {
    for (const auto &entry : kobalt::DictionaryManager::catalog()) {
        const QString zip = dropFolder + "/" + entry.id + ".zip";
        if (!QFileInfo::exists(zip))
            continue;
        if (QFileInfo::exists(dictionaryRoot + "/" + entry.id + "/installed.json")) {
            qInfo("kobalt-pb: dictionary %s already installed, ignoring %s", qPrintable(entry.id), qPrintable(zip));
            continue;
        }
        QString error;
        if (kobalt::DictionaryManager::installArchive(dictionaryRoot, entry, zip, &error)) {
            qInfo("kobalt-pb: installed dictionary %s (%s)", qPrintable(entry.id), qPrintable(entry.name));
            QFile::rename(zip, zip + ".installed");
        } else {
            qWarning("kobalt-pb: dictionary %s not installed: %s", qPrintable(entry.id), qPrintable(error));
        }
    }
}
// The log is a file on the device's flash (the launcher redirects stderr), and the page turn is
// sacred: the thread that turns pages must not write storage. Every line used to be written from
// the thread that logged it, and every non-debug line synced there too, which on the GUI thread
// is a wait of unknown length on the flash; the gravity sensor alone logs two such lines every
// few seconds while the device is held. Lines are stamped where they are logged and handed to
// this thread, which writes them in order and syncs.
void writeLog(const QByteArray &lines, bool sync) {
    const char *at = lines.constData();
    qint64 left = lines.size();
    while (left > 0) {
        const ssize_t written = ::write(2, at, size_t(left));
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            break;
        at += written;
        left -= written;
    }
    // Non-debug lines reach the flash at once: the firmware exports /mnt/ext1 over USB without
    // flushing what the apps still have in the page cache, and the tail of the log was lost.
    if (sync)
        fdatasync(2);
}
class LogWriter final : public QThread {
public:
    void post(const QByteArray &line, bool sync) {
        QMutexLocker lock(&mutex);
        if (finished) {
            writeLog(line, sync);
            return;
        }
        pending += line;
        syncPending = syncPending || sync;
        wake.wakeOne();
    }
    // Returns once every line posted so far is written (a fatal message, before abort()).
    void drain() {
        QMutexLocker lock(&mutex);
        while (!finished && (!pending.isEmpty() || writing))
            idle.wait(&mutex);
    }
    // The last lines of a run; later ones are written by the thread that logs them.
    void finish() {
        {
            QMutexLocker lock(&mutex);
            stopping = true;
            wake.wakeOne();
        }
        wait();
    }

protected:
    void run() override {
        QMutexLocker lock(&mutex);
        for (;;) {
            while (pending.isEmpty() && !stopping)
                wake.wait(&mutex);
            if (pending.isEmpty())
                break;
            QByteArray lines;
            lines.swap(pending);
            const bool sync = syncPending;
            syncPending = false;
            writing = true;
            lock.unlock();
            writeLog(lines, sync);
            lock.relock();
            writing = false;
            idle.wakeAll();
        }
        finished = true;
        idle.wakeAll();
    }

private:
    QMutex mutex;
    QWaitCondition wake, idle;
    QByteArray pending;
    bool syncPending = false, writing = false, stopping = false, finished = false;
};
LogWriter *logWriter = nullptr; // set in main() before any other thread exists, never deleted
struct LogWriterScope {
    LogWriterScope() {
        logWriter = new LogWriter;
        logWriter->start();
    }
    ~LogWriterScope() {
        logWriter->finish();
    }
};
void logHandler(QtMsgType type, const QMessageLogContext &, const QString &msg) {
    const char *level = type == QtDebugMsg ? "D" : type == QtInfoMsg ? "I" : type == QtWarningMsg ? "W" : "E";
    const QByteArray line = QTime::currentTime().toString("HH:mm:ss.zzz").toLatin1() + ' ' + level + ' ' +
                            msg.toLocal8Bit() + '\n';
    if (logWriter)
        logWriter->post(line, type != QtDebugMsg);
    else
        writeLog(line, type != QtDebugMsg);
    if (type == QtFatalMsg) {
        if (logWriter)
            logWriter->drain();
        abort();
    }
}
}

int main(int argc, char **argv) {
    using namespace kobalt;
    // The binary lives in applications/kobalt/; everything else sits next to it.
    const QString base = QFileInfo(QString::fromLocal8Bit(argv[0])).absolutePath();
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "pbink");
    if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR"))
        qputenv("QT_QPA_FONTDIR", (base + "/assets/fonts").toUtf8()); // flat dir of TTFs from the asset bundle
    // Ends after the last line of main(), on every way out of it.
    const LogWriterScope logging;
    qInstallMessageHandler(logHandler);
    qInfo("kobalt-pb %s starting from %s", KOBALT_VERSION, qPrintable(base));
    // Diagnostic entries carry the time since boot, which keeps counting through light sleep.
    // The process samples of the other hosts are left out: nothing reads them here.
    DiagnosticSystem diagnostics;
    diagnostics.bootMilliseconds = platform::processDiagnosticSystem().bootMilliseconds;
    setDiagnosticSystem(std::move(diagnostics));

    FontCopies copies;
    QApplication app(argc, argv);
    app.setApplicationName("Kobalt");
    app.setOrganizationName("Kobalt");
    app.setStyle("Fusion");
    // The firmware launches the last book a few seconds into the boot, behind its PIN screen,
    // and keeps the light off across a power-off that Kobalt kept dark (the timed one): switch
    // it on now that inkview is attached (the platform plugin's construction), before the fonts
    // and the rest of the start, which take several seconds the PIN screen would spend dark.
    pocketbook::restoreStandbyLight("launch");

    // Every book is a new process here, and verifying the bundle reads and hashes 46 MB of fonts
    // before the first frame (about 4 s on the Era Lite): record it, so that only a bundle that
    // changed is read again. Next to the binary, not in the profile: it describes this install.
    AssetBundle assets(base + "/assets", QStringLiteral(KOBALT_VERSION));
    assets.setVerificationRecord(base + "/assets-verified.json");
    QElapsedTimer assetClock;
    assetClock.start();
    const bool assetsAvailable = assets.available();
    qInfo("kobalt-pb: assets %s in %lld ms", assetsAvailable ? "verified" : "unavailable",
          static_cast<long long>(assetClock.elapsed()));
    if (!assetsAvailable || !ui::initializeInterfaceFonts(assets)) {
        qCritical("kobalt-pb: bundled assets are unavailable in %s/assets", qPrintable(base));
        return 2;
    }
    for (const auto &file : bundledFontFiles(assets))
        if (QFontDatabase::addApplicationFont(file) < 0)
            qWarning("kobalt-pb: cannot register font %s", qPrintable(file));

    const int ppi = qRound(app.primaryScreen()->physicalDotsPerInch());
    readingPpi.store(ppi > 0 ? ppi : 300);
    ui::setDensity(ppi > 0 ? ppi : 300);
    app.setFont(ui::interfaceFont());
    qInfo("kobalt-pb: screen %dx%d at %d ppi", app.primaryScreen()->geometry().width(),
          app.primaryScreen()->geometry().height(), ppi);

    qInfo("kobalt-pb: TLS %s, %s, %d CA certificates", QSslSocket::supportsSsl() ? "available" : "unavailable",
          qPrintable(QSslSocket::sslLibraryVersionString()),
          QSslConfiguration::defaultConfiguration().caCertificates().size());
    const QString profile = base + "/profile";
    QDir().mkpath(profile);
    QDir().mkpath(base + "/fonts");
    initializeDiagnostics(profile + "/diagnostics");
    QDir().mkpath(base + "/dictionaries");
    importDictionaries(base + "/dictionaries", profile + "/config/dictionaries");
    removeDictionaryReadmes(profile + "/config/dictionaries");
    pocketbook::Host host(storage::Paths{profile + "/config"},
                          engineFontInputs(assets, QStringLiteral(KOBALT_SYSTEM_FONT_DIRECTORY), base + "/fonts"), // user fonts: applications/kobalt/fonts
                          placeholderFontFamily(assets, copies));
    host.setGeometry(app.primaryScreen()->geometry());
    if (argc > 1)
        host.startPath = QString::fromLocal8Bit(argv[1]);
    else
        host.startMessage = QObject::tr("Kobalt %1 for PocketBook\n\nOpen an EPUB with \"Open with\" from the "
                                        "library.\n\nTap or press a key to leave.")
                                .arg(QStringLiteral(KOBALT_VERSION));
    // First frame after inkview's EVT_SHOW (the firmware re-applies its orientation right before it).
    auto *native = QGuiApplication::platformNativeInterface();
    QObject *notifier = nullptr;
    if (auto fn = native ? native->platformFunction("pbinkNotifier") : nullptr)
        notifier = reinterpret_cast<QObject *(*)()>(fn)();
    if (notifier)
        QObject::connect(notifier, SIGNAL(taskShown()), &host, SLOT(start()));
    QTimer::singleShot(notifier ? 2000 : 0, &host, &pocketbook::Host::start);
    const int rc = app.exec();
    qInfo("kobalt-pb: exit %d", rc);
    return rc;
}

// Kobalt on PocketBook (spike): opens the EPUB given on the command line with Kobalt's reader,
// drawn through inkview by the statically linked pbink platform plugin.
#include "host.h"
#include "assets/assets.h"
#include "assets/bundled_fonts.h"
#include "dictionaries/manager.h"
#include "interfaces/reader/ui/style/fonts.h"
#include "interfaces/reader/ui/style/scale.h"
#include "reading/reading_density.h"
#include "support/diagnostics.h"
#include <QApplication>
#include <unistd.h>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
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
void logHandler(QtMsgType type, const QMessageLogContext &, const QString &msg) {
    const char *level = type == QtDebugMsg ? "D" : type == QtInfoMsg ? "I" : type == QtWarningMsg ? "W" : "E";
    fprintf(stderr, "%s %s %s\n", qPrintable(QTime::currentTime().toString("HH:mm:ss.zzz")), level, qPrintable(msg));
    fflush(stderr);
    // Non-debug lines reach the flash at once: the firmware exports /mnt/ext1 over USB without
    // flushing what the apps still have in the page cache, and the tail of the log was lost.
    if (type != QtDebugMsg)
        fdatasync(2);
    if (type == QtFatalMsg)
        abort();
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
    qInstallMessageHandler(logHandler);
    qInfo("kobalt-pb %s starting from %s", KOBALT_VERSION, qPrintable(base));

    FontCopies copies;
    QApplication app(argc, argv);
    app.setApplicationName("Kobalt");
    app.setOrganizationName("Kobalt");
    app.setStyle("Fusion");

    AssetBundle assets(base + "/assets", QStringLiteral(KOBALT_VERSION));
    if (!assets.available() || !ui::initializeInterfaceFonts(assets)) {
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

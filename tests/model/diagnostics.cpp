#include "support/qt_compat.h"
#include "support/diagnostics.h"
#include "support/strings.h"
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QThread>
#include <vector>
#include <thread>
#include <atomic>
#include <dlfcn.h>
#include <cerrno>
#include <unistd.h>

// Count actual sync requests, while still performing the filesystem sync.
static std::atomic<int> syncCalls{0};
extern "C" int fdatasync(int descriptor) {
    ++syncCalls;
    static const auto sync = reinterpret_cast<int (*)(int)>(dlsym(RTLD_NEXT, "fsync"));
    if (!sync) qFatal("Cannot resolve the filesystem sync for the log test");
    return sync(descriptor);
}
static QByteArray read(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        qFatal("Cannot read log");
    return file.readAll();
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto device = kobalt::diagnosticDeviceFields(
        "PRIVATE-SERIAL,4.9.77,4.45.23646,4.9.77,4.9.77,00000000-0000-0000-0000-000000000390\n");
    if (device.value("hardware_uuid").toString() != "00000000-0000-0000-0000-000000000390" ||
        device.value("firmware").toString() != "4.45.23646" || device.size() != 2 ||
        QJsonDocument(device).toJson().contains("PRIVATE-SERIAL")) {
        qFatal("Device diagnostics lost model or disclosed serial");
    }
    if (!kobalt::diagnosticDeviceFields("PRIVATE-SERIAL,broken").isEmpty() ||
        !kobalt::diagnosticDeviceFields("PRIVATE-SERIAL,x,private firmware,x,x,PRIVATE-SERIAL").isEmpty()) {
        qFatal("Malformed device fields disclosed arbitrary data");
    }
    const auto code = QStringLiteral("error.native_publication_transfer_timed_out");
    if (kobalt::diagnosticErrorCode(kobalt::strings::get(code.toLatin1().constData())) != code ||
        kobalt::diagnosticErrorCode(code) != code || !kobalt::diagnosticErrorCode({}).isEmpty() ||
        kobalt::diagnosticErrorCode("Cannot read /private/book.epub") != "unknown")
        qFatal("Error codes lost or arbitrary error text disclosed");
    QTemporaryDir directory;
    if (!directory.isValid())
        qFatal("No temporary directory");
    if (kobalt::diagnosticsActive())
        qFatal("Diagnostics reported active before initialization");
    kobalt::initializeDiagnostics(directory.path());
    if (!kobalt::diagnosticsActive())
        qFatal("Initialized diagnostics reported inactive");
    const auto path = (directory.path() + "/" + "events.jsonl");
    auto writer = [] {
        for (int i = 0; i < 600; ++i) {
            errno = EDOM;
            // Padding makes the concurrent records pass the rotation limit
            // independently of optional memory samples.
            kobalt::diagnostic("render", {{"sequence", i}, {"padding", QString(64, 'p')}}, false);
            if (errno != EDOM)
                qFatal("Logging changed errno");
        }
    };
    std::thread first(writer), second(writer);
    first.join();
    second.join();
    kobalt::flushDiagnostics();
    for (const auto &file : {path, path + ".previous"}) {
        const auto bytes = read(file);
        if (bytes.isEmpty() || bytes.size() > 128 * 1024 || !bytes.endsWith('\n'))
            qFatal("Unbounded or incomplete log");
        for (const auto &line : bytes.split('\n')) {
            if (line.isEmpty())
                continue;
            const auto object = QJsonDocument::fromJson(line).object();
            if (!object.contains("event") || !object.contains("monotonic_ms") || !object.contains("pid"))
                qFatal("Invalid record");
        }
    }
    const auto before = read(path);
    kobalt::pauseDiagnostics(true);
    if (kobalt::diagnosticsActive())
        qFatal("Paused diagnostics reported active");
    kobalt::diagnostic("exported-storage");
    kobalt::flushDiagnostics();
    if (read(path) != before)
        qFatal("Wrote diagnostics during USB export");
    kobalt::pauseDiagnostics(false);
    kobalt::diagnostic("remounted-storage");
    kobalt::flushDiagnostics();
    if (read(path) == before)
        qFatal("Did not resume diagnostics after USB");
    const auto resumed = read(path);
    kobalt::diagnostic("oversized", {{"value", QString(5000, 'x')}});
    kobalt::flushDiagnostics();
    if (read(path) != resumed)
        qFatal("Oversized entry accepted");
    QFile::remove(path);
    const auto target = (directory.path() + "/" + "untouched");
    QFile sentinel(target);
    if (!sentinel.open(QIODevice::WriteOnly) || sentinel.write("sentinel") != 8)
        qFatal("Cannot write symlink target fixture");
    sentinel.close();
    if (::symlink(QFile::encodeName(target), QFile::encodeName(path)) != 0)
        qFatal("Cannot create test symlink");
    kobalt::diagnostic("must-not-follow");
    kobalt::flushDiagnostics();
    if (read(target) != "sentinel")
        qFatal("Followed log symlink");
    kobalt::initializeDiagnostics(target + "/not-a-directory");
    kobalt::diagnostic("unavailable");
    kobalt::flushDiagnostics();
    if (read(target) != "sentinel")
        qFatal("Logging failure damaged file");
    const int beforeNormal = syncCalls.load();
    QTemporaryDir quiet;
    kobalt::initializeDiagnostics(quiet.path(), false);
    const auto quietPath = quiet.path() + "/events.jsonl";
    if (kobalt::diagnosticsActive())
        qFatal("Quiet logging enabled expensive instrumentation");
    for (int i = 0; i < 20; ++i)
        kobalt::diagnostic("turn", {{"sequence", i}});
    // Ordinary activity must reach disk without a warning or explicit flush.
    QElapsedTimer normalWait;
    normalWait.start();
    while (!QFile::exists(quietPath) && normalWait.elapsed() < 3000)
        QThread::msleep(10);
    if (!QFile::exists(quietPath))
        qFatal("Normal activity was kept only in memory");
    QThread::msleep(50);
    if (syncCalls.load() != beforeNormal) qFatal("Normal activity requested a storage sync");
    QJsonObject observed;
    kobalt::setDiagnosticObserver([&](const QJsonObject &entry) { observed = entry; });
    if (!kobalt::diagnosticsActive())
        qFatal("Normal disk policy disabled the evaluation observer");
    kobalt::diagnostic("observed", {{"measurement", 42}});
    if (observed.value("measurement").toInt() != 42 || observed.contains("rss_kib"))
        qFatal("Observer lost fields or sampled memory on the caller");
    kobalt::setDiagnosticObserver({});
    QJsonObject sampledMemory;
    std::atomic<bool> sampled{false};
    kobalt::sampleDiagnosticMemory([&](QJsonObject fields) {
        if (QThread::currentThread() == app.thread()) qFatal("Memory sampled on the main thread");
        sampledMemory = std::move(fields);
        sampled.store(true);
    });
    QElapsedTimer sampleWait;
    sampleWait.start();
    while (!sampled.load() && sampleWait.elapsed() < 3000) QThread::msleep(10);
    if (!sampled.load() || !sampledMemory.value("rss_kib").isDouble() ||
        !sampledMemory.value("peak_rss_kib").isDouble()) qFatal("Explicit performance memory sample was lost");
    if (syncCalls.load() != beforeNormal) qFatal("Memory sampling requested a storage sync");
    kobalt::diagnostic("save", {{"ok", false}}, kobalt::DiagnosticSeverity::Error);
    QElapsedTimer syncWait;
    syncWait.start();
    while (syncCalls.load() == beforeNormal && syncWait.elapsed() < 3000) QThread::msleep(10);
    if (syncCalls.load() == beforeNormal) qFatal("Error batch did not request a sync");
    kobalt::flushDiagnostics();
    const int beforeBoundary = syncCalls.load();
    kobalt::flushDiagnostics();
    if (syncCalls.load() <= beforeBoundary) qFatal("Empty lifecycle flush did not sync prior batches");
    const auto quietBytes = read(quietPath);
    if (quietBytes.count("\"event\":\"turn\"") != 20 ||
        !quietBytes.contains("\"sequence\":19") || !quietBytes.contains("\"severity\":\"error\"") ||
        quietBytes.contains("rss_kib"))
        qFatal("Normal log lost activity fields or collected detailed memory data");
    if (sampledMemory.contains("kobalt_rss_kib") || sampledMemory.contains("kobalt_peak_kib"))
        qFatal("Kobalt's memory share was reported before its base");
    // The base is taken once; later samples report memory above it.
    kobalt::markMemoryBase();
    kobalt::markMemoryBase();
    kobalt::flushDiagnostics();
    std::vector<char> grown(8 * 1024 * 1024, 1);
    sampled.store(false);
    kobalt::sampleDiagnosticMemory([&](QJsonObject fields) {
        sampledMemory = std::move(fields);
        sampled.store(true);
    });
    sampleWait.restart();
    while (!sampled.load() && sampleWait.elapsed() < 3000) QThread::msleep(10);
    if (!sampled.load() || sampledMemory.value("kobalt_rss_kib").toDouble() < 4096 ||
        sampledMemory.value("kobalt_peak_kib").toDouble() < 4096 || grown.back() != 1)
        qFatal("Kobalt's memory share did not follow its base");
    QTemporaryDir deadline;
    kobalt::initializeDiagnostics(deadline.path(), false);
    const auto deadlinePath = deadline.path() + "/events.jsonl";
    kobalt::diagnosticInput();
    kobalt::diagnostic("deadline", {}, kobalt::DiagnosticSeverity::Warning);
    QElapsedTimer elapsed;
    elapsed.start();
    while (!QFile::exists(deadlinePath) && elapsed.elapsed() < 6000) {
        kobalt::diagnosticInput();
        QThread::msleep(20);
    }
    if (!QFile::exists(deadlinePath) || elapsed.elapsed() < 900)
        qFatal("Input did not defer diagnostics or prevented the maximum-delay flush");
    kobalt::pauseDiagnostics(true);
    qInfo("PASS: bounded concurrent logs, normal activity, error batches, observers, batching and safe files");
}

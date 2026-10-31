#include "scenario/player.h"
#include "support/diagnostics.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <cstdio>
#include <cstdlib>

// This check drives the player's failure paths without a reader or hardware.
namespace kobalt {
void setDiagnosticsSync(bool) {}
void setDiagnosticObserver(std::function<void(const QJsonObject &)>) {}
void diagnostic(const char *, QJsonObject, bool) {}
void sampleDiagnosticMemory(std::function<void(QJsonObject)> done) { done({}); }
namespace kosync {
static bool paused = false;
bool automaticPaused() { return paused; }
void pauseAutomatic(bool value) { paused = value; }
}
}
namespace {
void require(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    auto scenario = QJsonObject{{"id", "sleep.test"}, {"opt_in", true}, {"repeat", 1},
        {"needs", QJsonArray{"sleep-persistence"}},
        {"setup", QJsonArray{QJsonObject{{"sleep_persistence", QJsonObject{{"action", "configure"}}}}}},
        {"steps", QJsonArray{QJsonObject{{"sleep_persistence", QJsonObject{{"action", "drain"}}}}}},
        {"sample", QJsonObject{{"event", "sleep-persistence"}, {"field", "total_ms"}, {"stages", true}}}};
    const auto file = [&] { return QJsonObject{{"scenarios", QJsonArray{scenario}}}; };
    kobalt::scenario::Host host;
    bool overrideActive = false, fail = true;
    int cleanups = 0;
    host.sleepPersistence = [&](const QJsonObject &request) {
        if (request.isEmpty()) {
            overrideActive = false;
            ++cleanups;
            return QJsonObject();
        }
        if (request.value("action") == "configure") {
            overrideActive = true;
            return QJsonObject();
        }
        return fail ? QJsonObject{{"error", "write failed"}} :
            QJsonObject{{"total_ms", 5}, {"encode_write_ms", 4}, {"bytes", 1234}};
    };
    require(kobalt::scenario::run(file(), host).isEmpty(), "benchmark is opt-in");
    require(cleanups == 0, "excluded benchmark has no side effects");
    kobalt::scenario::Options options;
    options.filter = "sleep";
    auto results = kobalt::scenario::run(file(), host, options);
    require(results.size() == 1 && results[0].value("status") == "failed", "write failure retained");
    require(!overrideActive && cleanups == 1, "failure restores overrides");
    require(results[0].value("details").toArray().first().toObject().value("status") == "failed",
            "failed trial retained");
    fail = false;
    scenario["warmup"] = 1;
    results = kobalt::scenario::run(file(), host, options);
    require(results[0].value("status") == "completed" && !overrideActive && cleanups == 2,
            "success restores overrides");
    require(results[0].value("samples").toArray().size() == 1 &&
            results[0].value("details").toArray().size() == 2, "warmup details excluded from samples");
    require(!results[0].value("stages").toObject().contains("bytes"), "byte count is not a timing stage");
    host.sleepPersistence = {};
    results = kobalt::scenario::run(file(), host, options);
    require(results[0].value("status") == "skipped", "unsupported host skips benchmark");
}

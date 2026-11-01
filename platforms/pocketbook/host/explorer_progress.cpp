#include "explorer_progress.h"
#include "reading/reading_speed.h"
#include "support/main_thread.h"
#include "support/strings.h"
#include <inkview.h>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace kobalt::pocketbook {
namespace {
const char *databasePath = "/mnt/ext1/system/explorer-3/explorer-3.db";
const char *connectionName = "kobalt-explorer";
const QLatin1String cfiPrefix("pbr:/webkit?##epubcfi(");

// One short-lived connection per operation; the firmware's explorer keeps its own open.
class Connection {
public:
    Connection() : db(QSqlDatabase::addDatabase("QSQLITE", connectionName)) {
        db.setDatabaseName(QString::fromLatin1(databasePath));
        db.setConnectOptions("QSQLITE_BUSY_TIMEOUT=2000");
        if (!QFile::exists(QString::fromLatin1(databasePath)))
            error_ = QStringLiteral("no library database at ") + QLatin1String(databasePath);
        else if (!db.open())
            error_ = db.lastError().text();
    }
    ~Connection() {
        if (db.isOpen())
            db.close();
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase(connectionName);
    }
    bool ok() const {
        return error_.isEmpty();
    }
    const QString &error() const {
        return error_;
    }
    QSqlDatabase db;

private:
    QString error_;
};

QVariant selectOne(QSqlDatabase &db, const QString &sql, const QVariantList &binds, QString *error) {
    QSqlQuery query(db);
    query.prepare(sql);
    for (const auto &value : binds)
        query.addBindValue(value);
    if (!query.exec()) {
        *error = query.lastError().text();
        return {};
    }
    return query.next() ? query.value(0) : QVariant();
}

// The row id of the book at this path, or 0 when the library does not know it.
qint64 bookId(QSqlDatabase &db, const QString &bookPath, QString *error) {
    const QFileInfo file(bookPath);
    QString folder = file.absolutePath();
    while (folder.size() > 1 && folder.endsWith('/'))
        folder.chop(1);
    const QVariant folderId = selectOne(db, "SELECT id FROM folders WHERE name = ? LIMIT 1", {folder}, error);
    if (!error->isEmpty() || folderId.isNull())
        return 0;
    const QVariant book = selectOne(db, "SELECT book_id FROM files WHERE folder_id = ? AND filename = ? LIMIT 1",
                                    {folderId, file.fileName()}, error);
    return error->isEmpty() && !book.isNull() ? book.toLongLong() : 0;
}

// The id of the inkview profile in use (1, "default", when none is selected or found).
int profileId(QSqlDatabase &db, QString *error) {
    const char *name = GetCurrentProfile();
    if (!name || !*name)
        return 1;
    const QVariant id = selectOne(db, "SELECT id FROM profiles WHERE name = ? LIMIT 1", {QString::fromUtf8(name)}, error);
    return error->isEmpty() && !id.isNull() ? id.toInt() : 1;
}

struct Row {
    bool exists = false;
    QString position;
    qint64 positionTs = 0, opentime = 0;
    int cpage = -1, npage = 0, completed = 0;
};

Row readRow(QSqlDatabase &db, qint64 book, int profile, QString *error) {
    Row row;
    QSqlQuery query(db);
    query.prepare("SELECT position, position_ts, cpage, npage, completed, opentime FROM books_settings "
                  "WHERE bookid = ? AND profileid = ?");
    query.addBindValue(book);
    query.addBindValue(profile);
    if (!query.exec()) {
        *error = query.lastError().text();
        return row;
    }
    if (!query.next())
        return row;
    row.exists = true;
    row.position = query.value(0).toString();
    row.positionTs = query.value(1).toLongLong();
    row.cpage = query.value(2).isNull() ? -1 : query.value(2).toInt();
    row.npage = query.value(3).toInt();
    row.completed = query.value(4).toInt();
    row.opentime = query.value(5).toLongLong();
    return row;
}

ReadingProgress snapshot(const QString &identity, const Row &row) {
    ReadingProgress progress;
    progress.book = identity;
    progress.valid = row.exists;
    if (row.position.startsWith(cfiPrefix))
        progress.location = row.position;
    progress.modified = row.positionTs * 1000;
    progress.status = row.exists ? (row.completed ? 2 : 1) : 0;
    return progress;
}

// A consistent copy of the library database as it was before Kobalt first wrote to it.
void backupOnce(QSqlDatabase &db) {
    const QString backup = QString::fromLatin1(databasePath) + ".before-kobalt";
    if (QFile::exists(backup))
        return;
    QSqlQuery query(db);
    if (query.exec(QStringLiteral("VACUUM INTO '%1'").arg(backup))) {
        qInfo("kobalt-pb: library database backed up to %s", qPrintable(backup));
        return;
    }
    qWarning("kobalt-pb: library database backup by VACUUM INTO failed (%s), copying the file",
             qPrintable(query.lastError().text()));
    if (QFile::copy(QString::fromLatin1(databasePath), backup))
        qInfo("kobalt-pb: library database file copied to %s", qPrintable(backup));
    else
        qWarning("kobalt-pb: library database backup failed");
}

bool writeRow(QSqlDatabase &db, qint64 book, int profile, const Row &row, bool withPosition, QString *error) {
    if (!db.transaction()) {
        *error = db.lastError().text();
        return false;
    }
    QSqlQuery query(db);
    query.prepare(withPosition
                      ? "INSERT INTO books_settings (bookid, profileid, cpage, npage, completed, opentime, position, "
                        "position_ts) VALUES (?, ?, ?, ?, ?, ?, ?, ?) ON CONFLICT(bookid, profileid) DO UPDATE SET "
                        "cpage = excluded.cpage, npage = excluded.npage, completed = excluded.completed, "
                        "opentime = excluded.opentime, position = excluded.position, position_ts = excluded.position_ts"
                      : "INSERT INTO books_settings (bookid, profileid, cpage, npage, completed, opentime) "
                        "VALUES (?, ?, ?, ?, ?, ?) ON CONFLICT(bookid, profileid) DO UPDATE SET "
                        "cpage = excluded.cpage, npage = excluded.npage, completed = excluded.completed, "
                        "opentime = excluded.opentime");
    query.addBindValue(book);
    query.addBindValue(profile);
    query.addBindValue(row.cpage >= 0 ? QVariant(row.cpage) : QVariant());
    query.addBindValue(row.npage > 0 ? QVariant(row.npage) : QVariant());
    query.addBindValue(row.completed);
    query.addBindValue(row.opentime);
    if (withPosition) {
        query.addBindValue(row.position);
        query.addBindValue(row.positionTs);
    }
    if (!query.exec()) {
        *error = query.lastError().text();
        db.rollback();
        return false;
    }
    if (!db.commit()) {
        *error = db.lastError().text();
        db.rollback();
        return false;
    }
    return true;
}

class LibraryBridge final : public ReadingBridge {
public:
    LibraryBridge(QString identity, storage::Paths paths, qint64 book, int profile)
        : identity(std::move(identity)), paths(std::move(paths)), book(book), profile(profile) {
        speed.restore(storage::readRecord(this->paths.directory, "reading-speed", this->paths.readingSpeed()).object);
        active.invalidate();
        savedAt.invalidate();
    }
    ~LibraryBridge() override {
        saveSpeed(true);
    }
    bool deviceTransition() const override {
        return false;
    }
    bool writable() const override {
        return consecutiveErrors < 3; // three database errors in a row: local saves only for this book
    }
    ReadingProgress read(QString *error) const override {
        QString failure;
        Row row;
        Connection connection;
        if (connection.ok())
            row = readRow(connection.db, book, profile, &failure);
        else
            failure = connection.error();
        if (!failure.isEmpty()) {
            if (error)
                *error = failure;
            return {};
        }
        return snapshot(identity, row);
    }
    bool takeChange() override {
        return std::exchange(changed, false);
    }
    bool write(const ReadingProgress &expected, const QString &location, double, bool finished,
               const ReadingActivity &activity, ReadingProgress *verified, QString *error) override {
        auto fail = [&](const QString &message) {
            ++consecutiveErrors;
            qWarning("kobalt-pb: library progress NOT written: %s%s", qPrintable(message),
                     consecutiveErrors >= 3 ? " (giving up for this book)" : "");
            if (error)
                *error = message;
            return false;
        };
        QElapsedTimer clock;
        clock.start();
        Connection connection;
        if (!connection.ok())
            return fail(connection.error());
        QString failure;
        const Row before = readRow(connection.db, book, profile, &failure);
        if (!failure.isEmpty())
            return fail(failure);
        // As on Kobo: the row must still be the one the reader last saw; a newer position
        // (PBReader moved the book) is offered as a choice first.
        if (expected.valid ? !(snapshot(identity, before) == expected) : before.exists)
            return fail(strings::get("error.nickel_progress_changed_before_saving"));
        backupOnce(connection.db);
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        Row row = before;
        row.cpage = activity.page;
        row.npage = activity.pages;
        row.completed = finished ? 1 : 0;
        row.opentime = now;
        const bool withPosition = !location.isEmpty();
        if (withPosition) {
            row.position = location;
            row.positionTs = qMax(now, expected.modified / 1000 + 1);
        }
        if (!writeRow(connection.db, book, profile, row, withPosition, &failure))
            return fail(failure);
        const Row after = readRow(connection.db, book, profile, &failure);
        if (!failure.isEmpty())
            return fail(failure);
        const auto actual = snapshot(identity, after);
        if (!actual.valid || (withPosition && actual.location != location))
            return fail(strings::get("error.nickel_persistence_could_not_be_verified_local_position_was_retain"));
        consecutiveErrors = 0;
        *verified = actual;
        qInfo("kobalt-pb: library progress written: book %lld profile %d page %d/%d%s%s%s in %lld ms",
              static_cast<long long>(book), profile, activity.page + 1, activity.pages, finished ? ", completed" : "",
              withPosition ? ", position " : ", no position", withPosition ? qPrintable(location) : "",
              static_cast<long long>(clock.elapsed()));
        return true;
    }
    // As on Kobo: a finished book opened again is being read. Only the flag changes, so the row
    // stays the one the reader saw at open apart from the status ReadingBook now expects.
    bool markReading() override {
        mainthread::BlockingOperation operation("library-opening-status");
        auto fail = [](const QString &message) {
            qWarning("kobalt-pb: library status NOT reset: %s", qPrintable(message));
            return false;
        };
        Connection connection;
        if (!connection.ok())
            return fail(connection.error());
        QString failure;
        Row row = readRow(connection.db, book, profile, &failure);
        if (!failure.isEmpty())
            return fail(failure);
        if (!row.completed)
            return true;
        backupOnce(connection.db);
        row.completed = 0;
        if (!writeRow(connection.db, book, profile, row, false, &failure))
            return fail(failure);
        qInfo("kobalt-pb: library status reset: book %lld profile %d is no longer completed",
              static_cast<long long>(book), profile);
        return true;
    }
    // Kobalt's local pace estimator, as the standalone reader keeps it, with this bridge's own
    // active-time clock (ReadingBook runs it only without a bridge).
    void observeReading(const ReadingActivity &activity) override {
        if (!active.isValid())
            active.start();
        speed.observe(activity, activeBefore + active.elapsed());
    }
    bool estimates(const ReadingActivity &activity, ReadingEstimates *result) override {
        *result = speed.estimates(activity);
        return true;
    }
    void stopReading() override {
        if (active.isValid()) {
            activeBefore += active.elapsed();
            active.invalidate();
        }
        speed.pause();
        saveSpeed(false);
    }

private:
    void saveSpeed(bool force) {
        if (!speed.dirty() || (!force && savedAt.isValid() && savedAt.elapsed() < 60000))
            return;
        if (storage::writeRecord(paths.directory, "reading-speed", paths.readingSpeed(), speed.state())) {
            speed.saved();
            savedAt.restart();
        }
    }
    QString identity;
    storage::Paths paths;
    qint64 book;
    int profile;
    bool changed = true;
    int consecutiveErrors = 0;
    ReadingSpeed speed;
    QElapsedTimer active, savedAt;
    qint64 activeBefore = 0;
};
}

ReadingProgress readLibraryProgress(const QString &identity, const QString &bookPath) {
    Connection connection;
    if (!connection.ok()) {
        qWarning("kobalt-pb: library database unavailable: %s", qPrintable(connection.error()));
        return {};
    }
    QString error;
    const qint64 book = bookId(connection.db, bookPath, &error);
    const int profile = error.isEmpty() && book > 0 ? profileId(connection.db, &error) : 1;
    const Row row = error.isEmpty() && book > 0 ? readRow(connection.db, book, profile, &error) : Row();
    if (!error.isEmpty()) {
        qWarning("kobalt-pb: library progress NOT read: %s", qPrintable(error));
        return {};
    }
    if (book <= 0) {
        qInfo("kobalt-pb: book not in the library database, progress stays local: %s", qPrintable(bookPath));
        return {};
    }
    const auto progress = snapshot(identity, row);
    qInfo("kobalt-pb: library progress read: book %lld profile %d page %d/%d%s, position %s (ts %lld)",
          static_cast<long long>(book), profile, row.cpage + 1, row.npage, row.completed ? ", completed" : "",
          row.position.isEmpty() ? "none" : qPrintable(row.position), static_cast<long long>(row.positionTs));
    return progress;
}

std::unique_ptr<ReadingBridge> makeLibraryBridge(const QString &identity, const QString &bookPath,
                                                 const storage::Paths &paths) {
    Connection connection;
    if (!connection.ok())
        return {};
    QString error;
    const qint64 book = bookId(connection.db, bookPath, &error);
    if (!error.isEmpty() || book <= 0)
        return {};
    const int profile = profileId(connection.db, &error);
    if (!error.isEmpty())
        return {};
    return std::make_unique<LibraryBridge>(identity, paths, book, profile);
}
}

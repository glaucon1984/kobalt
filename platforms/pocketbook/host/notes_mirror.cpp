#include "notes_mirror.h"
#include "publication/zip_resource.h"
#include "storage/profile_database.h"
#include "support/main_thread.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUrl>
#include <QVariant>
#include <QVector>
#include <QXmlStreamReader>
#include <utility>

namespace kobalt::pocketbook {
namespace {
const char *libraryPath = "/mnt/ext1/system/explorer-3/explorer-3.db";
const char *notesPath = "/mnt/ext1/system/config/books.db";
// "KOBALT" and the version nibble of a name-based UUID: the rows this mirror owns.
const QString ownPattern = QStringLiteral("4B4F4241-4C54-5%");
constexpr int highlightKind = 1; // lists.kind and entries.kind in Kobalt's profile database

struct Entry {
    QString id, start, end, text, color, note;
    qint64 created = 0;
    int page = -1; // zero-based, in Kobalt's layout; -1 when not known
};
struct Book {
    QString key, path;
    qint64 revision = 0;
    QVector<Entry> entries;
};

// Highlight lists as they were mirrored last, per book key (GUI thread only).
QHash<QString, qint64> mirrored;

// Every book's highlight list, read on the profile's writer thread after its waiting saves.
bool readHighlights(const QString &directory, QVector<Book> *books, QString *error) {
    return storage::profileTransaction(
        directory,
        [books](QSqlDatabase &db) {
            QSqlQuery query(db);
            query.setForwardOnly(true);
            // Text as bytes, as Kobalt's stores read it (explicit UTF-8 whatever the provider).
            if (!query.exec(QStringLiteral(
                    "SELECT CAST(l.book AS BLOB), CAST(b.identity AS BLOB), l.revision, CAST(e.id AS BLOB), "
                    "CAST(e.start AS BLOB), CAST(e.end AS BLOB), CAST(e.text AS BLOB), CAST(e.created AS BLOB), "
                    "CAST(e.color AS BLOB), CAST(e.note AS BLOB) FROM lists l JOIN books b ON b.book = l.book "
                    "LEFT JOIN entries e ON e.book = l.book AND e.kind = l.kind WHERE l.kind = %1 "
                    "ORDER BY l.book, e.ordinal")
                                .arg(highlightKind)))
                return false;
            auto text = [&query](int column) { return QString::fromUtf8(query.value(column).toByteArray()); };
            while (query.next()) {
                const QString key = text(0);
                if (books->isEmpty() || books->last().key != key) {
                    Book book;
                    book.key = key;
                    const QUrl identity(text(1));
                    if (identity.isLocalFile())
                        book.path = identity.toLocalFile();
                    book.revision = query.value(2).toLongLong();
                    books->append(book);
                }
                if (query.value(3).isNull())
                    continue; // a list without entries
                Entry entry;
                entry.id = text(3);
                entry.start = text(4);
                entry.end = text(5);
                entry.text = text(6);
                const auto created = QDateTime::fromString(text(7), Qt::ISODate);
                entry.created = created.isValid() ? created.toSecsSinceEpoch() : QDateTime::currentSecsSinceEpoch();
                entry.color = text(8);
                entry.note = text(9);
                if (!entry.id.isEmpty() && !entry.end.isEmpty() && !entry.text.trimmed().isEmpty())
                    books->last().entries.append(entry);
            }
            return true;
        },
        error);
}

// The row id of a mirrored highlight: "KOBALT", then a hash of what the row holds, shaped as a
// name-based UUID like the firmware's own.
QString mirrorId(const Entry &entry) {
    QCryptographicHash hash(QCryptographicHash::Sha1);
    for (const QString *part : {&entry.id, &entry.start, &entry.end, &entry.text}) {
        hash.addData(part->toUtf8());
        hash.addData("\n", 1);
    }
    QByteArray bytes = QByteArray("KOBALT") + hash.result().left(10);
    bytes[6] = char((bytes.at(6) & 0x0f) | 0x50);
    bytes[8] = char((bytes.at(8) & 0x3f) | 0x80);
    const QString hex = QString::fromLatin1(bytes.toHex().toUpper());
    return hex.mid(0, 8) + '-' + hex.mid(8, 4) + '-' + hex.mid(12, 4) + '-' + hex.mid(16, 4) + '-' + hex.mid(20);
}

// ---- Positions: a CREngine XPointer of a highlight as PBReader's EPUB CFI ----

QByteArray readEntry(const QString &path, const QString &name, ZipResourceIndex &index, qint64 limit) {
    ZipResource stream;
    if (!stream.open(path, name, index, limit))
        return {};
    QByteArray data;
    while (stream.valid() && !stream.finished())
        data += stream.next();
    return stream.valid() && stream.finished() ? data : QByteArray();
}

// The spine as CREngine loads it: one DocFragment per XHTML spine item, in order.
struct Package {
    struct Fragment {
        QString entry; // the chapter's name in the archive
        int itemref = 0; // its place among the spine's itemref elements
    };
    bool ok = false;
    int spineStep = 6;
    QVector<Fragment> fragments;
};

Package readPackage(const QString &path, ZipResourceIndex &index) {
    Package package;
    QString opfPath;
    {
        QXmlStreamReader xml(readEntry(path, QStringLiteral("META-INF/container.xml"), index, 64 * 1024));
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement() && xml.name() == QLatin1String("rootfile")) {
                opfPath = xml.attributes().value(QLatin1String("full-path")).toString();
                break;
            }
        }
    }
    if (opfPath.isEmpty())
        return package;
    const int slash = opfPath.lastIndexOf('/');
    const QString base = slash < 0 ? QString() : opfPath.left(slash + 1);
    struct Item {
        QString href, type;
    };
    QHash<QString, Item> manifest;
    QStringList spine;
    QXmlStreamReader xml(readEntry(path, opfPath, index, 4 * 1024 * 1024));
    int depth = 0, elements = 0;
    QString section;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            ++depth;
            const auto attributes = xml.attributes();
            if (depth == 2) {
                ++elements;
                section = xml.name().toString();
                if (section == QLatin1String("spine"))
                    package.spineStep = 2 * elements;
            } else if (depth == 3 && section == QLatin1String("manifest") && xml.name() == QLatin1String("item")) {
                manifest.insert(attributes.value(QLatin1String("id")).toString(),
                                {attributes.value(QLatin1String("href")).toString(),
                                 attributes.value(QLatin1String("media-type")).toString()});
            } else if (depth == 3 && section == QLatin1String("spine") && xml.name() == QLatin1String("itemref")) {
                spine.append(attributes.value(QLatin1String("idref")).toString());
            }
        } else if (xml.isEndElement()) {
            --depth;
        }
    }
    for (int i = 0; i < spine.size(); ++i) {
        const auto item = manifest.constFind(spine[i]);
        if (item == manifest.constEnd() || item->type != QLatin1String("application/xhtml+xml"))
            continue;
        QString href = item->href;
        const int fragment = href.indexOf('#');
        if (fragment >= 0)
            href.truncate(fragment);
        package.fragments.append({QDir::cleanPath(base + QUrl::fromPercentEncoding(href.toUtf8())), i});
    }
    package.ok = !package.fragments.isEmpty();
    return package;
}

// A chapter's XHTML as the source has it: elements, and the text between them as one chunk each
// (what an EPUB CFI counts).
struct Node {
    bool text = false;
    QString value; // element name, or the chunk's text
    int parent = -1;
    QVector<int> children;
};
struct Chapter {
    bool ok = false;
    QVector<Node> nodes;
    int body = -1, bodyStep = 4;
};

// XHTML's named entities come from a DTD the stream reader does not load. One character each,
// so that offsets hold even for a name not listed here.
class HtmlEntities final : public QXmlStreamEntityResolver {
public:
    QString resolveUndeclaredEntity(const QString &name) override {
        static const QHash<QString, ushort> known{
            {"nbsp", 0xa0},   {"shy", 0xad},    {"mdash", 0x2014}, {"ndash", 0x2013},  {"hellip", 0x2026},
            {"laquo", 0xab},  {"raquo", 0xbb},  {"ldquo", 0x201c}, {"rdquo", 0x201d},  {"lsquo", 0x2018},
            {"rsquo", 0x2019}, {"copy", 0xa9},  {"reg", 0xae},     {"trade", 0x2122},  {"deg", 0xb0},
            {"times", 0xd7},  {"middot", 0xb7}, {"bull", 0x2022},  {"iexcl", 0xa1},    {"iquest", 0xbf},
            {"ordf", 0xaa},   {"ordm", 0xba},   {"aacute", 0xe1},  {"eacute", 0xe9},   {"iacute", 0xed},
            {"oacute", 0xf3}, {"uacute", 0xfa}, {"ntilde", 0xf1},  {"uuml", 0xfc},     {"Aacute", 0xc1},
            {"Eacute", 0xc9}, {"Iacute", 0xcd}, {"Oacute", 0xd3},  {"Uacute", 0xda},   {"Ntilde", 0xd1},
            {"Uuml", 0xdc},   {"agrave", 0xe0}, {"egrave", 0xe8},  {"ccedil", 0xe7},   {"euro", 0x20ac}};
        return QString(QChar(known.value(name, 0xfffd)));
    }
};

QString localName(const QStringRef &qualified) {
    const int colon = qualified.lastIndexOf(':');
    return (colon < 0 ? qualified : qualified.mid(colon + 1)).toString();
}

Chapter readChapter(const QByteArray &data) {
    Chapter chapter;
    if (data.isEmpty())
        return chapter;
    QXmlStreamReader xml(data);
    xml.setNamespaceProcessing(false); // an undeclared prefix is not this reader's business
    HtmlEntities entities;
    xml.setEntityResolver(&entities);
    auto &nodes = chapter.nodes;
    int current = -1;
    while (!xml.atEnd()) {
        switch (xml.readNext()) {
        case QXmlStreamReader::StartElement: {
            Node node;
            node.value = localName(xml.qualifiedName());
            node.parent = current;
            const int id = nodes.size();
            nodes.append(node);
            if (current >= 0)
                nodes[current].children.append(id);
            current = id;
            break;
        }
        case QXmlStreamReader::EndElement:
            if (current >= 0)
                current = nodes[current].parent;
            break;
        case QXmlStreamReader::Characters: {
            if (current < 0)
                break;
            const auto &siblings = nodes[current].children;
            if (!siblings.isEmpty() && nodes[siblings.last()].text) {
                nodes[siblings.last()].value += xml.text();
                break;
            }
            Node node;
            node.text = true;
            node.value = xml.text().toString();
            node.parent = current;
            const int id = nodes.size();
            nodes.append(node);
            nodes[current].children.append(id);
            break;
        }
        default:
            break;
        }
    }
    if (xml.hasError() || nodes.isEmpty())
        return chapter;
    int elements = 0;
    for (int child : nodes[0].children) {
        if (nodes[child].text)
            continue;
        ++elements;
        if (nodes[child].value.compare(QLatin1String("body"), Qt::CaseInsensitive) == 0) {
            chapter.body = child;
            chapter.bodyStep = 2 * elements;
            break;
        }
    }
    chapter.ok = chapter.body >= 0;
    return chapter;
}

// "/body[1]/DocFragment[8]/body[1]/p[20]/text()[1].114": the chapter, the steps below its body
// and the offset (in code points, as CREngine counts).
struct Pointer {
    struct Step {
        QString name;
        int index = 1;
        bool text = false;
    };
    bool ok = false;
    int fragment = 0;
    QVector<Step> steps;
    int offset = 0;
};

Pointer parsePointer(const QString &xpointer) {
    static const QRegularExpression stepPattern(QStringLiteral("^([^\\[\\]]+)(?:\\[(\\d+)\\])?$"));
    Pointer pointer;
    QString path = xpointer;
    const int dot = path.lastIndexOf('.');
    if (dot > path.lastIndexOf('/')) {
        bool number = false;
        const int offset = path.mid(dot + 1).toInt(&number);
        if (number && offset >= 0) {
            pointer.offset = offset;
            path.truncate(dot);
        }
    }
    const QStringList parts = path.split('/', Qt::SkipEmptyParts);
    if (parts.size() < 3)
        return pointer;
    for (int i = 0; i < parts.size(); ++i) {
        const auto match = stepPattern.match(parts[i]);
        if (!match.hasMatch())
            return pointer;
        Pointer::Step step;
        step.name = match.captured(1);
        step.index = match.captured(2).isEmpty() ? 1 : match.captured(2).toInt();
        step.text = step.name == QLatin1String("text()");
        if (step.index < 1)
            return pointer;
        if (i == 0 || i == 2) {
            if (step.name != QLatin1String("body"))
                return pointer;
        } else if (i == 1) {
            if (step.name != QLatin1String("DocFragment"))
                return pointer;
            pointer.fragment = step.index;
        } else {
            if (step.text && i != parts.size() - 1)
                return pointer;
            pointer.steps.append(step);
        }
    }
    pointer.ok = true;
    return pointer;
}

bool blank(const QString &text) {
    for (const QChar c : text)
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f')
            return false;
    return true;
}

// Where a pointer lands in the source: the CFI steps below the chapter's body and, for a text
// target, the chunk (the engine drops blank chunks, or keeps them: both readings are offered).
struct Place {
    QString steps;
    int chunk = -1;
};

QVector<Place> places(const Chapter &chapter, const Pointer &pointer) {
    const auto &nodes = chapter.nodes;
    int parent = chapter.body;
    QString steps;
    for (const auto &step : pointer.steps) {
        const auto &children = nodes[parent].children;
        if (step.text) {
            QVector<Place> found;
            for (const bool skipBlank : {true, false}) {
                int elements = 0, seen = 0;
                for (int child : children) {
                    if (!nodes[child].text) {
                        ++elements;
                        continue;
                    }
                    if (skipBlank && blank(nodes[child].value))
                        continue;
                    if (++seen == step.index) {
                        if (found.isEmpty() || found.last().chunk != child)
                            found.append({steps + QStringLiteral("/%1").arg(2 * elements + 1), child});
                        break;
                    }
                }
            }
            return found;
        }
        int elements = 0, seen = 0, next = -1;
        for (int child : children) {
            if (nodes[child].text)
                continue;
            ++elements;
            if (nodes[child].value.compare(step.name, Qt::CaseInsensitive) == 0 && ++seen == step.index) {
                next = child;
                break;
            }
        }
        // Not there: one of the engine's own wrappers (autoBoxing, floatBox...) or a structure
        // it repaired. The caller falls back to the chapter.
        if (next < 0)
            return {};
        steps += QStringLiteral("/%1").arg(2 * elements);
        parent = next;
    }
    return {{steps, -1}};
}

// A chunk as the engine may hold it: whitespace runs as one space, with or without the chunk's
// leading whitespace, or untouched. Each code point remembers its UTF-16 offset in the source,
// which is what a CFI counts.
struct Reading {
    QVector<uint> points;
    QVector<int> source;
    int sourceLength = 0;
    QString slice(int from, int to) const {
        from = qBound(0, from, points.size());
        to = qBound(from, to, points.size());
        return QString::fromUcs4(points.constData() + from, to - from);
    }
    int sourceStart(int offset) const {
        return offset < points.size() ? source[qMax(0, offset)] : sourceLength;
    }
    int sourceEnd(int offset) const {
        if (offset <= 0)
            return sourceStart(0);
        const int last = qMin(offset, points.size()) - 1;
        return last < 0 ? 0 : source[last] + (points[last] > 0xffff ? 2 : 1);
    }
};

Reading reading(const QString &text, int variant) {
    Reading result;
    result.sourceLength = text.size();
    bool lastSpace = variant == 1; // variant 1 drops leading whitespace
    for (int i = 0; i < text.size(); ++i) {
        uint point = text[i].unicode();
        const int at = i;
        if (text[i].isHighSurrogate() && i + 1 < text.size() && text[i + 1].isLowSurrogate()) {
            point = QChar::surrogateToUcs4(text[i], text[i + 1]);
            ++i;
        }
        const bool space = point == ' ' || point == '\t' || point == '\n' || point == '\r' || point == '\f';
        if (variant != 2 && space) {
            if (lastSpace)
                continue;
            point = ' ';
        }
        lastSpace = variant != 2 && space;
        result.points.append(point);
        result.source.append(at);
    }
    return result;
}

QString normal(QString text) {
    text.remove(QChar(0xad));
    return text.simplified();
}

class Anchors {
public:
    explicit Anchors(QString path) : path(std::move(path)) {
    }
    // PBReader's begin and end locations of a highlight. `exact` is false when only its chapter
    // could be confirmed. False when not even the chapter is known.
    bool locate(const Entry &entry, QString *begin, QString *end, bool *exact) {
        if (!packageRead) {
            packageRead = true;
            package = readPackage(path, index);
        }
        const Pointer from = parsePointer(entry.start), to = parsePointer(entry.end);
        if (!package.ok || !from.ok || from.fragment < 1 || from.fragment > package.fragments.size())
            return false;
        QString first, last;
        *exact = to.ok && to.fragment >= from.fragment && to.fragment <= package.fragments.size() &&
                 resolve(entry, from, to, &first, &last);
        if (!*exact)
            first = last = prefix(from.fragment);
        // PBReader's page and offset are not known: Kobalt's page for Notes, and offset zero.
        const int page = qMax(0, entry.page);
        *begin = QStringLiteral("pbr:/word?page=%1&offs=0#epubcfi(").arg(page) + first + ')';
        *end = QStringLiteral("pbr:/char?page=%1&over=0#epubcfi(").arg(page) + last + ')';
        return true;
    }

private:
    const Chapter &chapter(int fragment) {
        auto it = chapters.find(fragment);
        if (it == chapters.end())
            it = chapters.insert(fragment,
                                 readChapter(readEntry(path, package.fragments[fragment - 1].entry, index,
                                                       16 * 1024 * 1024)));
        return *it;
    }
    QString prefix(int fragment) {
        return QStringLiteral("/%1/%2!/%3")
            .arg(package.spineStep)
            .arg(2 * (package.fragments[fragment - 1].itemref + 1))
            .arg(chapter(fragment).bodyStep);
    }
    // Both ends in the source, confirmed against the highlight's text.
    bool resolve(const Entry &entry, const Pointer &from, const Pointer &to, QString *first, QString *last) {
        const Chapter &startChapter = chapter(from.fragment);
        const Chapter &endChapter = chapter(to.fragment);
        if (!startChapter.ok || !endChapter.ok)
            return false;
        const auto starts = places(startChapter, from), ends = places(endChapter, to);
        if (starts.isEmpty() || ends.isEmpty())
            return false;
        const QString wanted = normal(entry.text);
        const QString startPrefix = prefix(from.fragment), endPrefix = prefix(to.fragment);
        auto textStep = [](const QString &steps, int offset, bool always) {
            return offset > 0 || always ? steps + QStringLiteral(":%1").arg(offset) : steps;
        };
        // One chunk holds the whole highlight: its slice is the text.
        if (from.fragment == to.fragment) {
            for (const auto &place : starts) {
                if (place.chunk < 0)
                    continue;
                for (const auto &other : ends) {
                    if (other.chunk != place.chunk)
                        continue;
                    for (int variant = 0; variant < 3; ++variant) {
                        const Reading text = reading(startChapter.nodes[place.chunk].value, variant);
                        if (to.offset > text.points.size() || from.offset >= to.offset ||
                            normal(text.slice(from.offset, to.offset)) != wanted)
                            continue;
                        *first = startPrefix + textStep(place.steps, text.sourceStart(from.offset), false);
                        *last = endPrefix + textStep(other.steps, text.sourceEnd(to.offset), true);
                        return true;
                    }
                }
            }
        }
        // Several chunks: the text begins with the rest of the first and ends with the head of
        // the last. An end on an element (an image) has nothing to compare and is taken as it is.
        QString begin, finish;
        bool compared = false;
        for (const auto &place : starts) {
            if (!begin.isEmpty())
                break;
            if (place.chunk < 0) {
                begin = startPrefix + place.steps;
                break;
            }
            for (int variant = 0; variant < 3 && begin.isEmpty(); ++variant) {
                const Reading text = reading(startChapter.nodes[place.chunk].value, variant);
                const QString rest = normal(text.slice(from.offset, text.points.size()));
                if (from.offset > text.points.size() || rest.isEmpty() || !wanted.startsWith(rest))
                    continue;
                begin = startPrefix + textStep(place.steps, text.sourceStart(from.offset), false);
                compared = true;
            }
        }
        for (const auto &place : ends) {
            if (!finish.isEmpty())
                break;
            if (place.chunk < 0) {
                finish = endPrefix + place.steps;
                break;
            }
            for (int variant = 0; variant < 3 && finish.isEmpty(); ++variant) {
                const Reading text = reading(endChapter.nodes[place.chunk].value, variant);
                const QString head = normal(text.slice(0, to.offset));
                if (to.offset > text.points.size() || head.isEmpty() || !wanted.endsWith(head))
                    continue;
                finish = endPrefix + textStep(place.steps, text.sourceEnd(to.offset), true);
                compared = true;
            }
        }
        if (begin.isEmpty() || finish.isEmpty() || !compared)
            return false;
        *first = begin;
        *last = finish;
        return true;
    }

    QString path;
    ZipResourceIndex index;
    Package package;
    bool packageRead = false;
    QHash<int, Chapter> chapters;
};

// ---- The firmware's databases ----

// One short-lived connection per run; the firmware keeps its own open.
class Database {
public:
    Database(const char *path, const char *connection) : name(QString::fromLatin1(connection)) {
        db = QSqlDatabase::addDatabase("QSQLITE", name);
        db.setDatabaseName(QString::fromLatin1(path));
        db.setConnectOptions("QSQLITE_BUSY_TIMEOUT=2000");
        if (!QFile::exists(QString::fromLatin1(path)))
            error = QStringLiteral("no database at ") + QLatin1String(path);
        else if (!db.open())
            error = db.lastError().text();
    }
    ~Database() {
        if (writing)
            db.rollback();
        if (db.isOpen())
            db.close();
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase(name);
    }
    bool ok() const {
        return error.isEmpty();
    }
    // Runs a statement; a failure is kept as the connection's error.
    bool run(QSqlQuery &query, const QString &sql, const QVariantList &binds = {}) {
        if (!ok())
            return false;
        if (!query.prepare(sql)) {
            error = query.lastError().text() + QStringLiteral(" (") + sql.left(48) + ')';
            return false;
        }
        for (const auto &value : binds)
            query.addBindValue(value);
        if (!query.exec()) {
            error = query.lastError().text() + QStringLiteral(" (") + sql.left(48) + ')';
            return false;
        }
        return true;
    }
    bool run(const QString &sql, const QVariantList &binds = {}) {
        QSqlQuery query(db);
        return run(query, sql, binds);
    }
    QVariant value(const QString &sql, const QVariantList &binds = {}) {
        QSqlQuery query(db);
        return run(query, sql, binds) && query.next() ? query.value(0) : QVariant();
    }
    QString name, error;
    QSqlDatabase db;
    bool writing = false;
};

struct LibraryBook {
    bool found = false;
    QString hash, title, author, folder, name;
    qint64 size = 0;
};

// The library's record of the book at this path: the id the firmware knows it by, and what its
// row in books.db would hold.
LibraryBook libraryBook(Database &library, const QString &path) {
    LibraryBook book;
    const QFileInfo file(path);
    book.folder = file.absolutePath();
    while (book.folder.size() > 1 && book.folder.endsWith('/'))
        book.folder.chop(1);
    book.name = file.fileName();
    QSqlQuery query(library.db);
    if (!library.run(query,
                     QStringLiteral("SELECT f.fast_hash, f.size, b.title, b.author FROM files f "
                                    "JOIN folders d ON d.id = f.folder_id LEFT JOIN books_impl b ON b.id = f.book_id "
                                    "WHERE d.name = ? AND f.filename = ? LIMIT 1"),
                     {book.folder, book.name}) ||
        !query.next())
        return book;
    const QVariant hash = query.value(0);
    book.hash = (hash.type() == QVariant::ByteArray ? QString::fromLatin1(hash.toByteArray().toHex())
                                                    : hash.toString())
                    .toUpper();
    book.size = query.value(1).toLongLong();
    book.title = query.value(2).toString();
    book.author = query.value(3).toString();
    if (book.title.isEmpty())
        book.title = file.completeBaseName();
    book.found = book.hash.size() == 32;
    return book;
}

// The ids books.db gives its type and tag names (read, not assumed: they are rows of its own).
struct Layout {
    int bookType = -1, markType = -1, storage = -1;
    QHash<QString, int> tags;
    int tag(const char *name) const {
        return tags.value(QLatin1String(name), -1);
    }
};

bool readLayout(Database &notes, Layout *layout) {
    QSqlQuery query(notes.db);
    if (!notes.run(query, QStringLiteral("SELECT OID, TypeName FROM TypeNames")))
        return false;
    while (query.next()) {
        const QString name = query.value(1).toString();
        if (name == QLatin1String("type.book"))
            layout->bookType = query.value(0).toInt();
        else if (name == QLatin1String("obj.book_mark"))
            layout->markType = query.value(0).toInt();
        else if (name == QLatin1String("stor.local_fs"))
            layout->storage = query.value(0).toInt();
    }
    if (!notes.run(query, QStringLiteral("SELECT OID, TagName FROM TagNames")))
        return false;
    while (query.next())
        layout->tags.insert(query.value(1).toString(), query.value(0).toInt());
    for (const char *name : {"bm.book_mark", "bm.type", "bm.quotation", "bm.note", "bm.color", "ro.authors",
                             "ro.mime-type", "ro.time-added", "doc.book-title", "doc.mime-type"})
        if (layout->tag(name) < 0) {
            notes.error = QStringLiteral("books.db has no tag ") + QLatin1String(name);
            return false;
        }
    if (layout->bookType < 0 || layout->markType < 0 || layout->storage < 0) {
        notes.error = QStringLiteral("books.db has not the expected types");
        return false;
    }
    return true;
}

// A consistent copy of books.db as it was before Kobalt first wrote to it.
void backupOnce(Database &notes) {
    const QString backup = QString::fromLatin1(notesPath) + ".before-kobalt";
    if (QFile::exists(backup))
        return;
    QSqlQuery query(notes.db);
    if (query.exec(QStringLiteral("VACUUM INTO '%1'").arg(backup))) {
        qInfo("kobalt-pb: notes database backed up to %s", qPrintable(backup));
        return;
    }
    qWarning("kobalt-pb: notes database backup by VACUUM INTO failed (%s), copying the file",
             qPrintable(query.lastError().text()));
    if (!QFile::copy(QString::fromLatin1(notesPath), backup))
        qWarning("kobalt-pb: notes database backup failed");
}

bool beginWriting(Database &notes) {
    if (notes.writing)
        return true;
    backupOnce(notes);
    if (!notes.db.transaction()) {
        notes.error = notes.db.lastError().text();
        return false;
    }
    notes.writing = true;
    return true;
}

// The book's row in books.db: by the firmware's id, else by its file, else 0.
qint64 findBook(Database &notes, const Layout &layout, const LibraryBook &book) {
    const QVariant byHash = notes.value(QStringLiteral("SELECT OID FROM Items WHERE TypeID = ? AND HashUUID = ? "
                                                       "COLLATE NOCASE LIMIT 1"),
                                        {layout.bookType, book.hash});
    if (!byHash.isNull())
        return byHash.toLongLong();
    const QString folder = book.folder + '/';
    const QVariant byFile =
        notes.value(QStringLiteral("SELECT f.BookID FROM Files f JOIN Paths p ON p.OID = f.PathID "
                                   "WHERE p.Path = ? AND f.Name = ? LIMIT 1"),
                    {folder, book.name});
    return byFile.isNull() ? 0 : byFile.toLongLong();
}

// The row PBReader makes when it first opens a book, as far as Notes needs it.
qint64 createBook(Database &notes, const Layout &layout, const LibraryBook &book) {
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const QVariant author = book.author.isEmpty() ? QVariant() : QVariant(book.author);
    QSqlQuery query(notes.db);
    if (!notes.run(query,
                   QStringLiteral("INSERT INTO Items(ParentID, TypeID, State, TimeAlt, HashUUID) "
                                  "VALUES(NULL, ?, 0, ?, ?)"),
                   {layout.bookType, now, book.hash}))
        return 0;
    const qint64 item = query.lastInsertId().toLongLong();
    if (!notes.run(QStringLiteral("INSERT INTO Books(OID, Title, Authors, TimeAdd) VALUES(?, ?, ?, ?)"),
                   {item, book.title, author, now}))
        return 0;
    // The search index over Books has no triggers. A provider without FTS5 leaves it out: the
    // book is then missing from a search only.
    if (notes.run(QStringLiteral("SAVEPOINT kobalt_fts"))) {
        QSqlQuery index(notes.db);
        index.prepare(QStringLiteral("INSERT INTO FTSBooks(rowid, Title, Authors) VALUES(?, ?, ?)"));
        index.addBindValue(item);
        index.addBindValue(book.title);
        index.addBindValue(author);
        if (!index.exec()) {
            qWarning("kobalt-pb: notes mirror: search index not updated: %s", qPrintable(index.lastError().text()));
            notes.run(QStringLiteral("ROLLBACK TO kobalt_fts"));
        }
        notes.run(QStringLiteral("RELEASE kobalt_fts"));
    }
    const QString folder = book.folder + '/';
    if (!notes.run(QStringLiteral("INSERT OR IGNORE INTO Paths(StorID, Path) VALUES(?, ?)"), {layout.storage, folder}))
        return 0;
    const QVariant path =
        notes.value(QStringLiteral("SELECT OID FROM Paths WHERE StorID = ? AND Path = ?"), {layout.storage, folder});
    if (path.isNull() ||
        !notes.run(QStringLiteral("INSERT OR IGNORE INTO Files(BookID, PathID, Name, Len, Ord) VALUES(?, ?, ?, ?, 0)"),
                   {item, path, book.name, book.size}))
        return 0;
    const QVector<QPair<const char *, QVariant>> tags{{"ro.authors", author},
                                                      {"ro.mime-type", QStringLiteral("EPUB")},
                                                      {"ro.time-added", QString::number(now)},
                                                      {"doc.book-title", book.title},
                                                      {"doc.mime-type", QStringLiteral("application/epub+zip")}};
    for (const auto &tag : tags)
        if (!tag.second.isNull() &&
            !notes.run(QStringLiteral("INSERT INTO Tags(ItemID, TagID, Val, TimeEdt) VALUES(?, ?, ?, 0)"),
                       {item, layout.tag(tag.first), tag.second}))
            return 0;
    return item;
}

QString json(const QJsonObject &object) {
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}
// The firmware's reader writes "cian" and, on colour panels, "yellow".
QString firmwareColor(const QString &color) {
    return color == QLatin1String("yellow") ? QStringLiteral("yellow") : QStringLiteral("cian");
}
QString firmwareType(const Entry &entry) {
    return entry.note.isEmpty() ? QStringLiteral("highlight") : QStringLiteral("note");
}

bool writeTag(Database &notes, qint64 item, int tag, const QString &value, qint64 time) {
    return notes.run(QStringLiteral("INSERT OR REPLACE INTO Tags(ItemID, TagID, Val, TimeEdt) VALUES(?, ?, ?, ?)"),
                     {item, tag, value, time});
}
// What can change on a row whose id still matches: the colour and the typed note.
bool writeAppearance(Database &notes, const Layout &layout, qint64 item, const Entry &entry) {
    if (!writeTag(notes, item, layout.tag("bm.type"), firmwareType(entry), entry.created) ||
        !writeTag(notes, item, layout.tag("bm.color"), firmwareColor(entry.color), entry.created))
        return false;
    if (entry.note.isEmpty())
        return notes.run(QStringLiteral("DELETE FROM Tags WHERE ItemID = ? AND TagID = ?"),
                         {item, layout.tag("bm.note")});
    return writeTag(notes, item, layout.tag("bm.note"), json({{"text", entry.note}}), entry.created);
}

// The page a row's position names, -1 when it has none.
int storedPage(const QString &quotation) {
    static const QRegularExpression pattern(QStringLiteral("^pbr:/\\w+\\?page=(\\d+)&"));
    const auto match =
        pattern.match(QJsonDocument::fromJson(quotation.toUtf8()).object().value("begin").toString());
    return match.hasMatch() ? match.captured(1).toInt() : -1;
}
// A row's JSON value with another page in the positions under `keys`.
QString repaged(const QString &value, int page, std::initializer_list<const char *> keys) {
    static const QRegularExpression pattern(QStringLiteral("^(pbr:/\\w+\\?page=)\\d+"));
    QJsonObject object = QJsonDocument::fromJson(value.toUtf8()).object();
    for (const char *key : keys) {
        QString position = object.value(QLatin1String(key)).toString();
        const auto match = pattern.match(position);
        if (match.hasMatch())
            position = match.captured(1) + QString::number(page) + position.mid(match.capturedEnd());
        object.insert(QLatin1String(key), position);
    }
    return json(object);
}

struct Counts {
    int added = 0, updated = 0, removed = 0, coarse = 0, skipped = 0, books = 0, created = 0;
    bool any() const {
        return added || updated || removed;
    }
};

// One book's highlights into books.db. False on a database error (the caller rolls back).
bool mirrorBook(Database &notes, const Layout &layout, const Book &book, const LibraryBook &known, Counts *counts) {
    qint64 item = findBook(notes, layout, known);
    if (!notes.ok())
        return false;
    struct Row {
        qint64 item = 0;
        QString type, color, note, quotation, bookmark;
    };
    QHash<QString, Row> existing;
    if (item > 0) {
        QSqlQuery query(notes.db);
        if (!notes.run(query,
                       QStringLiteral(
                           "SELECT i.OID, i.HashUUID, "
                           "(SELECT Val FROM Tags WHERE ItemID = i.OID AND TagID = ?), "
                           "(SELECT Val FROM Tags WHERE ItemID = i.OID AND TagID = ?), "
                           "(SELECT Val FROM Tags WHERE ItemID = i.OID AND TagID = ?), "
                           "(SELECT Val FROM Tags WHERE ItemID = i.OID AND TagID = ?), "
                           "(SELECT Val FROM Tags WHERE ItemID = i.OID AND TagID = ?) "
                           "FROM Items i WHERE i.ParentID = ? AND i.TypeID = ? AND i.HashUUID LIKE ?"),
                       {layout.tag("bm.type"), layout.tag("bm.color"), layout.tag("bm.note"),
                        layout.tag("bm.quotation"), layout.tag("bm.book_mark"), item, layout.markType, ownPattern}))
            return false;
        while (query.next()) {
            Row row;
            row.item = query.value(0).toLongLong();
            row.type = query.value(2).toString();
            row.color = query.value(3).toString();
            row.note = QJsonDocument::fromJson(query.value(4).toString().toUtf8()).object().value("text").toString();
            row.quotation = query.value(5).toString();
            row.bookmark = query.value(6).toString();
            existing.insert(query.value(1).toString().toUpper(), row);
        }
    }
    Anchors anchors(book.path);
    bool counted = false;
    auto touched = [&] {
        if (!std::exchange(counted, true))
            ++counts->books;
    };
    for (const Entry &entry : book.entries) {
        const QString id = mirrorId(entry);
        const auto row = existing.constFind(id);
        if (row != existing.constEnd()) {
            // The same highlight: only its colour or note can differ, and its page with the layout.
            const Row found = *row;
            existing.erase(existing.find(id));
            const bool sameLook = found.type == firmwareType(entry) && found.color == firmwareColor(entry.color) &&
                                  found.note == entry.note;
            const bool samePage = entry.page < 0 || storedPage(found.quotation) == entry.page;
            if (sameLook && samePage)
                continue;
            if (!beginWriting(notes) || (!sameLook && !writeAppearance(notes, layout, found.item, entry)))
                return false;
            if (!samePage &&
                (!writeTag(notes, found.item, layout.tag("bm.quotation"),
                           repaged(found.quotation, entry.page, {"begin", "end"}), entry.created) ||
                 !writeTag(notes, found.item, layout.tag("bm.book_mark"),
                           repaged(found.bookmark, entry.page, {"anchor"}), entry.created)))
                return false;
            ++counts->updated;
            touched();
            continue;
        }
        QString begin, end;
        bool exact = false;
        if (!anchors.locate(entry, &begin, &end, &exact)) {
            ++counts->skipped;
            continue;
        }
        if (!beginWriting(notes))
            return false;
        if (item <= 0) {
            item = createBook(notes, layout, known);
            if (item <= 0)
                return false;
            ++counts->created;
        }
        QSqlQuery query(notes.db);
        if (!notes.run(query,
                       QStringLiteral("INSERT INTO Items(ParentID, TypeID, State, TimeAlt, HashUUID) "
                                      "VALUES(?, ?, 0, ?, ?)"),
                       {item, layout.markType, entry.created, id}))
            return false;
        const qint64 mark = query.lastInsertId().toLongLong();
        if (!writeTag(notes, mark, layout.tag("bm.book_mark"), json({{"anchor", begin}, {"created", entry.created}}),
                      entry.created) ||
            !writeTag(notes, mark, layout.tag("bm.quotation"),
                      json({{"begin", begin}, {"end", end}, {"text", entry.text}}), entry.created) ||
            !writeAppearance(notes, layout, mark, entry))
            return false;
        ++counts->added;
        if (!exact)
            ++counts->coarse;
        touched();
    }
    // What is left was removed or edited in Kobalt.
    for (const Row &row : qAsConst(existing)) {
        if (!beginWriting(notes) ||
            !notes.run(QStringLiteral("DELETE FROM Tags WHERE ItemID = ?"), {row.item}) ||
            !notes.run(QStringLiteral("DELETE FROM Items WHERE OID = ?"), {row.item}))
            return false;
        ++counts->removed;
        touched();
    }
    return true;
}
}

QStringList highlightPositions(const QString &profileDirectory, const QString &bookPath) {
    mainthread::BlockingOperation operation("notes-positions");
    QVector<Book> books;
    QStringList positions;
    if (!readHighlights(profileDirectory, &books, nullptr))
        return positions;
    const QString wanted = QFileInfo(bookPath).absoluteFilePath();
    for (const Book &book : qAsConst(books))
        if (!book.path.isEmpty() && QFileInfo(book.path).absoluteFilePath() == wanted)
            for (const Entry &entry : book.entries)
                positions.append(entry.start);
    return positions;
}

bool mirrorHighlights(const QString &profileDirectory, const char *reason, const QString &openBook,
                      const QHash<QString, int> &pages) {
    mainthread::BlockingOperation operation("notes-mirror");
    QElapsedTimer clock;
    clock.start();
    QVector<Book> books;
    QString error;
    if (!readHighlights(profileDirectory, &books, &error)) {
        qWarning("kobalt-pb: notes mirror (%s): highlights NOT read: %s", reason, qPrintable(error));
        return false;
    }
    const QString open = openBook.isEmpty() || pages.isEmpty() ? QString() : QFileInfo(openBook).absoluteFilePath();
    QVector<const Book *> changed;
    for (Book &book : books) {
        // The open book's pages are of the layout on screen: looked at even when its list is
        // the one mirrored last, since a new layout moves them.
        const bool paged = !open.isEmpty() && !book.path.isEmpty() && QFileInfo(book.path).absoluteFilePath() == open;
        if (paged)
            for (Entry &entry : book.entries)
                entry.page = pages.value(entry.start, -1);
        if (paged || mirrored.value(book.key, -1) != book.revision)
            changed.append(&book);
    }
    if (changed.isEmpty())
        return false;
    Counts counts;
    QVector<const Book *> done;
    {
        Database library(libraryPath, "kobalt-notes-library");
        Database notes(notesPath, "kobalt-notes");
        Layout layout;
        if (!library.ok() || !notes.ok() || !notes.run(QStringLiteral("PRAGMA foreign_keys = ON")) ||
            !readLayout(notes, &layout)) {
            qWarning("kobalt-pb: notes mirror (%s): NOT run: %s", reason,
                     qPrintable(library.ok() ? notes.error : library.error));
            return false;
        }
        for (const Book *book : qAsConst(changed)) {
            // A book the library does not list (or no file at all) has no row to hang notes on.
            const LibraryBook known = book->path.isEmpty() ? LibraryBook() : libraryBook(library, book->path);
            if (!library.ok())
                break;
            if (known.found && !mirrorBook(notes, layout, *book, known, &counts)) {
                if (notes.ok())
                    notes.error = QStringLiteral("the book's row could not be made");
                break;
            }
            done.append(book);
        }
        if (!library.ok() || !notes.ok() || (notes.writing && !notes.db.commit())) {
            qWarning("kobalt-pb: notes mirror (%s): NOT written: %s", reason,
                     qPrintable(!library.ok() ? library.error
                                              : notes.ok() ? notes.db.lastError().text() : notes.error));
            return false; // the connection rolls back; every book is tried again next time
        }
        notes.writing = false;
    }
    for (const Book *book : qAsConst(done))
        mirrored.insert(book->key, book->revision);
    if (counts.any() || counts.skipped)
        qInfo("kobalt-pb: notes mirror (%s): %d added (%d by chapter only), %d updated, %d removed, %d not placed, "
              "%d book(s), %d new book row(s), %lld ms",
              reason, counts.added, counts.coarse, counts.updated, counts.removed, counts.skipped, counts.books,
              counts.created, static_cast<long long>(clock.elapsed()));
    else
        qInfo("kobalt-pb: notes mirror (%s): %d list(s) checked, nothing to write, %lld ms", reason,
              int(changed.size()), static_cast<long long>(clock.elapsed()));
    return counts.any();
}
}

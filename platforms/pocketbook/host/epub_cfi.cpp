#include "epub_cfi.h"
#include "engine/crengine/column_view.h"
#include "engine/crengine/reader_model.h"
#include "support/strings.h"
#include <lvdocview.h>
#include <lvtinydom.h>
#include <QHash>
#include <QRegularExpression>
#include <QVector>
#include <QXmlStreamReader>
#include <mutex>

namespace kobalt::pocketbook {
namespace {
const QLatin1String locationPrefix("pbr:/webkit?##epubcfi(");
const QLatin1String fragmentPrefix("_doc_fragment_");

QString qstring(const lString32 &s) {
    return QString::fromUtf8(UnicodeToUtf8(s).c_str());
}
lString32 cre(const QString &s) {
    return Utf8ToUnicode(s.toUtf8().constData());
}

QByteArray readEntry(ldomDocument *doc, const QString &path, int limit) {
    auto container = doc->getContainer();
    if (container.isNull())
        return {};
    auto stream = container->OpenStream(cre(path).c_str(), LVOM_READ);
    if (stream.isNull())
        return {};
    const int size = int(qMin<lvsize_t>(stream->GetSize(), lvsize_t(limit)));
    QByteArray data(size, '\0');
    lvsize_t read = 0;
    if (size > 0 && stream->Read(data.data(), lvsize_t(size), &read) != LVERR_OK)
        return {};
    data.truncate(int(read));
    return data;
}

// Per document: the CFI step of <spine> inside <package> (6 for the usual metadata, manifest,
// spine order) and the step of <body> inside each chapter's <html> (4 with a <head>, 2 without).
struct PackageInfo {
    ldomDocument *doc = nullptr;
    int spineStep = 6;
    QHash<QString, int> bodySteps;
};
std::mutex infoMutex;
PackageInfo info;

PackageInfo &packageInfo(ldomDocument *doc) {
    if (info.doc == doc)
        return info;
    info = PackageInfo{doc, 6, {}};
    QString opfPath;
    {
        QXmlStreamReader xml(readEntry(doc, QStringLiteral("META-INF/container.xml"), 64 * 1024));
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement() && xml.name() == QLatin1String("rootfile")) {
                opfPath = xml.attributes().value(QLatin1String("full-path")).toString();
                break;
            }
        }
    }
    if (!opfPath.isEmpty()) {
        QXmlStreamReader xml(readEntry(doc, opfPath, 4 * 1024 * 1024));
        int depth = 0, elements = 0;
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement()) {
                ++depth;
                if (depth == 2) {
                    ++elements;
                    if (xml.name() == QLatin1String("spine")) {
                        info.spineStep = 2 * elements;
                        break;
                    }
                }
            } else if (xml.isEndElement()) {
                --depth;
            }
        }
    }
    return info;
}

int bodyStep(PackageInfo &package, const QString &source) {
    const auto it = package.bodySteps.constFind(source);
    if (it != package.bodySteps.constEnd())
        return *it;
    const QByteArray head = readEntry(package.doc, source, 64 * 1024).toLower();
    const int body = head.indexOf("<body");
    const int step = body >= 0 && head.left(body).indexOf("<head") < 0 ? 2 : 4;
    package.bodySteps.insert(source, step);
    return step;
}

// CREngine's own wrappers around source nodes: transparent for CFI steps.
bool transparent(ldomNode *node) {
    return node->isElement() && (node->isNodeName("autoBoxing") || node->isNodeName("floatBox") ||
                                 node->isNodeName("inlineBox") || node->isNodeName("mathBox"));
}
// Content CREngine generated from CSS (::before/::after): not in the source at all.
bool generated(ldomNode *node) {
    return node->isElement() && node->isNodeName("pseudoElem");
}
// The children of `element` as the source document had them.
void sourceChildren(ldomNode *element, QVector<ldomNode *> &out) {
    for (int i = 0; i < element->getChildCount(); ++i) {
        auto *child = element->getChildNode(i);
        if (generated(child))
            continue;
        if (transparent(child))
            sourceChildren(child, out);
        else
            out.append(child);
    }
}
ldomNode *fragmentBody(ldomNode *fragment) {
    for (int i = 0; i < fragment->getChildCount(); ++i) {
        auto *child = fragment->getChildNode(i);
        if (child->isElement() && child->isNodeName("body"))
            return child;
    }
    return nullptr;
}
int spineIndex(ldomNode *fragment) {
    const QString id = qstring(fragment->getAttributeValue("id"));
    if (!id.startsWith(fragmentPrefix))
        return -1;
    bool ok = false;
    const int index = id.mid(fragmentPrefix.size()).toInt(&ok);
    return ok ? index : -1;
}
ldomNode *fragmentAt(ldomDocument *doc, int spine) {
    auto *body = doc->nodeFromXPath(U"/body");
    if (!body)
        return nullptr;
    for (int i = 0; i < body->getChildCount(); ++i) {
        auto *node = body->getChildNode(i);
        if (node->isElement() && node->isNodeName("DocFragment") && spineIndex(node) == spine)
            return node;
    }
    return nullptr;
}
QString pointer(ldomNode *node, int offset) {
    return qstring(ldomXPointer(node, offset).toString());
}
}

QString PocketBookPositionCodec::encode(const ReaderModel &model, QString *error) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return QString();
    };
    if (error)
        error->clear();
    if (!model.engine_)
        return fail(strings::get("error.no_reading_position"));
    auto *doc = model.engine_->getDocument();
    const auto xpointer = doc->createXPointer(cre(model.anchor_));
    auto *target = xpointer.getNode();
    if (!target)
        return fail(strings::get("error.invalid_source_position"));
    auto *fragment = target;
    while (fragment && !fragment->isNodeName("DocFragment"))
        fragment = fragment->getParentNode();
    if (!fragment)
        return fail(strings::get("error.position_has_no_source_chapter"));
    const int spine = spineIndex(fragment);
    auto *body = fragmentBody(fragment);
    if (spine < 0 || !body)
        return fail(strings::get("error.position_has_no_source_chapter"));
    std::lock_guard<std::mutex> guard(infoMutex);
    auto &package = packageInfo(doc);
    QString cfi = QStringLiteral("/%1/%2!/%3")
                      .arg(package.spineStep)
                      .arg(2 * (spine + 1))
                      .arg(bodyStep(package, qstring(fragment->getAttributeValue("Source"))));
    // The source ancestors of the target below the chapter's body, top down.
    QVector<ldomNode *> chain;
    for (auto *node = target; node && node != body && node != fragment; node = node->getParentNode())
        if (!transparent(node) && !generated(node))
            chain.prepend(node);
    auto *parent = body;
    for (auto *node : chain) {
        QVector<ldomNode *> children;
        sourceChildren(parent, children);
        const int index = children.indexOf(node);
        if (index < 0)
            return fail(strings::get("error.source_position_is_outside_its_anchor"));
        int elementsBefore = 0;
        for (int i = 0; i < index; ++i)
            if (children[i]->isElement())
                ++elementsBefore;
        if (node->isElement()) {
            cfi += QStringLiteral("/%1").arg(2 * (elementsBefore + 1));
        } else {
            cfi += QStringLiteral("/%1").arg(2 * elementsBefore + 1);
            if (node == target && xpointer.getOffset() > 0)
                cfi += QStringLiteral(":%1").arg(xpointer.getOffset());
        }
        parent = node;
    }
    return locationPrefix + cfi + ')';
}

QString PocketBookPositionCodec::decode(const ReaderModel &model, const QString &location, QString *error) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return QString();
    };
    if (error)
        error->clear();
    if (!model.engine_ || location.isEmpty() || location.size() > 4096)
        return fail(strings::get("error.no_supported_native_reading_position"));
    const int start = location.indexOf(QLatin1String("epubcfi("));
    if (start < 0)
        return fail(strings::get("error.no_supported_native_reading_position"));
    QString cfi = location.mid(start + 8);
    const int close = cfi.lastIndexOf(')');
    if (close >= 0)
        cfi.truncate(close);
    static const QRegularExpression assertion(QStringLiteral("\\[[^\\]]*\\]"));
    static const QRegularExpression step(QStringLiteral("^(\\d+)(?::(\\d+))?"));
    cfi.remove(assertion);
    const int bang = cfi.indexOf('!');
    const QStringList spineSteps = (bang < 0 ? cfi : cfi.left(bang)).split('/', Qt::SkipEmptyParts);
    const QStringList chapterSteps = bang < 0 ? QStringList() : cfi.mid(bang + 1).split('/', Qt::SkipEmptyParts);
    if (spineSteps.size() < 2)
        return fail(strings::get("error.no_supported_native_reading_position"));
    const auto item = step.match(spineSteps[1]);
    const int itemStep = item.hasMatch() ? item.captured(1).toInt() : 0;
    if (itemStep <= 0 || itemStep % 2)
        return fail(strings::get("error.no_supported_native_reading_position"));
    auto *doc = model.engine_->getDocument();
    auto *fragment = fragmentAt(doc, itemStep / 2 - 1);
    if (!fragment)
        return fail(strings::get("error.native_chapter_is_not_in_the_publication_spine"));
    auto *body = fragmentBody(fragment);
    if (!body)
        return pointer(fragment, 0);
    // The first chapter step is the <body> itself, whatever its number.
    auto *node = body;
    for (int i = 1; i < chapterSteps.size(); ++i) {
        const auto match = step.match(chapterSteps[i]);
        if (!match.hasMatch())
            return fail(strings::get("error.no_supported_native_reading_position"));
        const int number = match.captured(1).toInt();
        const int offset = match.captured(2).isEmpty() ? 0 : match.captured(2).toInt();
        QVector<ldomNode *> children;
        sourceChildren(node, children);
        if (number % 2 == 0) {
            // The number / 2 th element child.
            int seen = 0;
            ldomNode *element = nullptr;
            for (auto *child : children)
                if (child->isElement() && ++seen == number / 2) {
                    element = child;
                    break;
                }
            if (!element)
                return fail(strings::get("error.native_text_position_was_not_found"));
            node = element;
            if (i == chapterSteps.size() - 1)
                return pointer(node, 0);
        } else {
            // The text after (number - 1) / 2 elements; the next element when the engine kept
            // no text node there.
            const int before = (number - 1) / 2;
            int seen = 0;
            for (auto *child : children) {
                if (child->isElement()) {
                    if (seen == before)
                        return pointer(child, 0);
                    ++seen;
                } else if (seen == before && child->isText()) {
                    return pointer(child, qBound(0, offset, int(child->getText().length())));
                }
            }
            return fail(strings::get("error.native_text_position_was_not_found"));
        }
    }
    return pointer(node, 0);
}

QString PocketBookPositionCodec::chapterStart(const QString &location) {
    const int bang = location.indexOf('!');
    if (bang < 0 || location.indexOf(QLatin1String("epubcfi(")) < 0)
        return {};
    return location.left(bang + 1) + ')';
}

PositionCodec positionCodec() {
    return {PocketBookPositionCodec::decode, PocketBookPositionCodec::encode, PocketBookPositionCodec::chapterStart};
}
}

#pragma once
#include "platform/api/position_codec.h"
#include <QString>

namespace kobalt {
class ReaderModel;
}

namespace kobalt::pocketbook {
// PBReader (the firmware's EPUB reader) keeps its place in the library database as an EPUB CFI
// with its own prefix: "pbr:/webkit?##epubcfi(/6/22!/4/2/174/1:52)". This codec maps such a
// location to a position in Kobalt's engine document and back, the way the Kobo target maps
// Nickel's locations: the spine step (/6 here, read from the OPF), the spine item (2 * (index +
// 1), CREngine keeps one DocFragment per spine item, numbered "_doc_fragment_<index>"), the
// chapter's body (/4 when the XHTML has a head, /2 otherwise), then the element path as the
// source document had it (CREngine's own autoBoxing/floatBox/inlineBox wrappers are skipped,
// generated pseudo elements dropped) and, for a text node, its character offset. CREngine
// normalises whitespace, so the offset is approximate; the element path is exact as long as
// the engine kept the source structure, which it does for ordinary XHTML.
// The codec runs on the engine worker, like Kobo's.
class PocketBookPositionCodec {
public:
    static QString decode(const ReaderModel &model, const QString &location, QString *error);
    static QString encode(const ReaderModel &model, QString *error);
    // The chapter part of a location: "pbr:/webkit?##epubcfi(/6/22!)", decoded as its body start.
    static QString chapterStart(const QString &location);
};
PositionCodec positionCodec();
}

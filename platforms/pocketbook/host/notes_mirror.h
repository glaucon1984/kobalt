#pragma once
#include <QHash>
#include <QString>
#include <QStringList>

namespace kobalt::pocketbook {
// Kobalt's highlights in the firmware's Notes section. Notes lists what the firmware's reader
// (PBReader) keeps in /mnt/ext1/system/config/books.db: one Items row of type obj.book_mark per
// note under the book's own Items row, with its content in Tags (bm.type "highlight" or "note",
// bm.quotation {"begin","end","text"}, bm.book_mark {"anchor","created"}, bm.color, and bm.note
// {"text"} for a typed note). Notes shows the text; it does not open the book.
//
// The mirror is one way, Kobalt's profile database to books.db, and it only ever touches rows it
// made: their HashUUID starts with the bytes "KOBALT" (4B4F4241-4C54-5...), followed by a hash
// of the highlight's id, range and text, so an edited highlight is a new row and an unchanged one
// needs no work. PBReader's own notes are never read or changed.
//
// The book's row is the firmware's: its HashUUID is files.fast_hash of the library database
// (explorer-3.db), which lists every book on the device, so a book PBReader never opened gets
// its row (Items, Books, FTSBooks, Paths, Files and the title, author and type tags) from there.
//
// Positions are PBReader's: "pbr:/word?page=0&offs=0#epubcfi(/6/16!/4/42/1:3)", PBReader's own
// page and offset on it, which Kobalt cannot know, and a standard EPUB CFI. The CFI is resolved
// against the EPUB's own XHTML, not the engine's document, and checked against the highlight's
// text; a highlight whose place cannot be confirmed gets its chapter as position, so that it
// still shows in Notes with the right text.
//
// The page Notes shows is the position's `page` + 1. For the book that is open the host passes
// Kobalt's own page of each highlight (start position to zero-based page in the current layout,
// from the reader's engine), so Notes shows the page number Kobalt shows; a row whose page
// changed with the layout is rewritten. A highlight without a known page says page 1 until its
// book is next open at a run.
//
// Runs at standby, at a USB plug and at close, on the calling thread (a storage wait: the caller
// names why it may block). A book is looked at again only when its highlight list changed since
// the last run in this process, the open book whenever pages are passed. books.db is copied once
// to books.db.before-kobalt before the first write. True when it wrote.
bool mirrorHighlights(const QString &profileDirectory, const char *reason, const QString &openBook = {},
                      const QHash<QString, int> &pages = {});
// The start positions of the highlights of the book at `bookPath`, for that page lookup.
QStringList highlightPositions(const QString &profileDirectory, const QString &bookPath);
}

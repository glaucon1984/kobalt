#pragma once
#include "reading/reading_bridge.h"
#include "storage/reader_storage.h"
#include <QString>
#include <memory>

namespace kobalt::pocketbook {
// The firmware's library database (/mnt/ext1/system/explorer-3/explorer-3.db, table
// books_settings) as Kobalt's native reading progress, the role Nickel's database plays on Kobo:
// the home screen and the library read the book's progress (cpage 0-based, npage, completed,
// opentime) from it, and PBReader keeps its own place in `position` (an EPUB CFI, see
// epub_cfi.h) with `position_ts`. The book's row is found through folders.name (the directory,
// no trailing slash) and files.filename, the profile through inkview's GetCurrentProfile()
// (falls back to 1). Writes are an UPSERT of those columns only, so a favourite mark survives;
// the database is copied once to explorer-3.db.before-kobalt before the first write.
//
// What the library knows: PBReader's position as `location` (only its EPUB CFI form, another
// format is left out so it never pulls the reader elsewhere), position_ts as `modified` (ms),
// status 2 when completed else 1; invalid when the library has no row for the book. `book` is
// Kobalt's identity, as its checkpoints expect.
ReadingProgress readLibraryProgress(const QString &identity, const QString &bookPath);
// The bridge Kobalt's reader saves through at open, standby/USB and close, and polls for a
// position PBReader moved (then it offers the choice, as on Kobo). Null when the library does
// not list the book: the reader then saves locally only. Keeps Kobalt's local reading-speed
// record going, as the standalone reader does, since the firmware has no pace estimator.
std::unique_ptr<ReadingBridge> makeLibraryBridge(const QString &identity, const QString &bookPath,
                                                 const storage::Paths &paths);
}

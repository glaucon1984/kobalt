#pragma once
#include "platform/api/reader_host_services.h"

namespace kobalt::pocketbook {
// Host services for the PocketBook: inkview battery, frontlight and network, physical page
// buttons, Kobalt's widget keyboard, the lock-screen bitmap as sleep screen.
ReaderHostServices readerServices();
// Your own StarDict dictionaries (2.4.2, HTML, .dict.dz), unpacked in folders here. Kobalt lists
// them beside the downloaded ones and writes its indexes beside their files when a book opens,
// as Kobo does with .adds/kobalt/dictionaries. Catalogue zips dropped here are installed at start.
inline constexpr auto userDictionaryDirectory = "/mnt/ext1/applications/kobalt/dictionaries";
}

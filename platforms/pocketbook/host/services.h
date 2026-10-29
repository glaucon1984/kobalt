#pragma once
#include "platform/api/reader_host_services.h"

namespace kobalt::pocketbook {
// Host services for the PocketBook: inkview battery, frontlight and network, physical page
// buttons, Kobalt's widget keyboard, the lock-screen bitmap as sleep screen.
ReaderHostServices readerServices();
}

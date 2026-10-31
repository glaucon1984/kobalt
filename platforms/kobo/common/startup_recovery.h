#ifndef KOBALT_STARTUP_RECOVERY_H
#define KOBALT_STARTUP_RECOVERY_H
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
// Only unfinished Kobalt initialization counts, never a later reader crash.
bool kobalt_startup_remove(const char *path);
bool kobalt_startup_begin(const char *path);
bool kobalt_startup_complete(const char *path);
// The removal notice survives uninstall and normal shutdown. A completed
// initialization clears it along with the unfinished-attempt marker.
bool kobalt_startup_removed(const char *notice);
bool kobalt_startup_recovered(const char *path, const char *notice);
#ifdef __cplusplus
}
#endif
#endif

# Storage compatibility

Kobalt keeps saved reading data apart from disposable caches. Storage formats have their own versions. They do not follow app releases.

## Rules

- Never change a format version only because the release version changed.
- Hosts choose the storage directory. `storage::Paths` owns the shared file names.
- JSON files use UTF-8. Writes use `QSaveFile`, so a file is only replaced by a complete new file.
- A compatible optional field keeps the current format version. Older records load with an explicit default for it.
- Edits keep the supported optional fields of a record.
- A change to required fields, their meaning or their location needs a new format version, a migration that keeps user data, and regression tests.
- Never overwrite or delete unsupported or damaged saved data. Profile preparation moves unusable files into `backups/` and starts from defaults. Other reads reject the file and leave it in place.
- Do not assume that older builds keep unknown fields or can safely write newer schemas. The area files say what older builds do where it is known.
- Cache invalidation rebuilds disposable data only. It never clears settings, bookmarks, annotations or reading history.
- Saved data never goes in a cache directory.

## Saved-data versions

| Data | Version |
| --- | --- |
| `reader-settings.json` | `schema: 1` |
| `kosync.json` | `schema: 1` |
| `kobalt.sqlite` | `PRAGMA user_version = 4` |
| Book records (`book_state`) and `profile_state` records | format 1 |
| Legacy JSON book state | `schema: 1` |
| Legacy JSON bookmarks and highlights | `version: 1` |

Cache versions change more often. The current values are in `platforms/cache-policy.cmake` (`rendering_version`) and in `documentCacheChecksum` (parser format). See [Disposable caches](storage/caches.md).

## Storage areas

Read only the file for the area you change.

| File | Covers |
| --- | --- |
| [Settings](storage/settings.md) | `reader-settings.json`, global preferences, retired keys, `config.json` |
| [Book state](storage/book-state.md) | Per-book checkpoints, `main_text_marks`, `image_views`, `page_estimate`, sleep privacy |
| [Annotations and words](storage/annotations.md) | Bookmarks, pins, custom pin SVGs, highlights, notes, saved words, annotation exports |
| [Database and recovery](storage/database.md) | `kobalt.sqlite`, the profile connection, durability, backups, reading sessions, migration, `backups/` recovery |
| [Disposable caches](storage/caches.md) | Blaze parsed books, rendering policy, layout snapshots, sleep backdrops, preset previews, retired caches |
| [Sync and Android storage](storage/sync-and-android.md) | `kosync.json`, Android preferences, resource folders, deferred KOSync |

## Plugin data

[Lua plugins](plugins.md#saved-data) save data in their own folder, separate from saved reading data and from disposable caches: `<plugin data root>/<plugin id>/`, where the root is `.adds/kobalt/plugin-data` on Kobo, `plugin-data` in the profile folder on desktop and `plugin-data` in the external files folder on Android. `global.json` holds a plugin's shared data and `books/<key>.json` one book's data, where `<key>` is the SHA-256 hex of `BookmarkStore::bookKey`. Each file is `{ "format": 1, "values": { ... } }` with UTF-8 JSON, written with `QSaveFile` on the plugin thread, and only when its values changed. A shared store holds at most 256 KiB and a book store 64 KiB, counted as UTF-8 key bytes plus compact JSON value bytes.

A file that cannot be read, is not a JSON object, has another `format`, has no `values` object or is over its limit is moved whole into `backups/` beside it, named after the file with a UTC timestamp, and logged with `plugin-data-backup`. The plugin then starts that store empty. When the move fails, the file stays where it is and that store refuses every `set` until the next book opens, so a save cannot replace it. Kobalt never deletes or rewrites such a file in place. Plugin data never changes reading data, settings or caches, and app upgrades do not reset it.

`plugins_enabled` and `plugins_disabled` are optional global preferences in `reader-settings.json`. `plugins_enabled` is a Boolean; missing or invalid values use true, and false loads no plugin. `plugins_disabled` is an array of plugin ids that do not load; a value that is not an array of strings is removed, and it is saved sorted. Both apply when the next book opens. Ids of plugins that are no longer installed stay in the list. `plugins_answer_first` is an optional Boolean global preference; missing or invalid values use false, and true opens a lookup on a plugin's card when a plugin answers. It applies from the next lookup. All three keep preference schema 1.

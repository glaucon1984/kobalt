# PocketBook port notes

The development log of the PocketBook port, kept as it was written (2026-10-25 to 2026-10-29),
when the port lived in its own folder next to an export of Kobalt and was built against an
unmodified upstream tree. It is dense by design: every device finding, dead end and timing is
here, for whoever (or whatever agent) works on the port next. The user-facing text is
[Kobalt on PocketBook](pocketbook.md); the build is in [Building](building.md#pocketbook).

Paths in this log refer to that earlier layout. In this repository:

| Then | Now |
| --- | --- |
| `kobalt-pocketbook/` (the host) | `platforms/pocketbook/host/` and `platforms/pocketbook/CMakeLists.txt` |
| `plugin/` (the pbink platform plugin) | `platforms/pocketbook/pbink/` |
| `tools/` (build, install, device scripts) | `platforms/pocketbook/tools/`, SDK production in `tools/sdk/` |
| `tools/kobalt-patches.py` (changes to Kobalt's sources) | commits on the `pocketbook` branch |
| `kobalt-check/` | `platforms/pocketbook/tools/kobalt-check/` |
| `device/`, `kobalt.app` | `platforms/pocketbook/device/` |
| `hello/`, `tools/remote-build.sh`, `tools/build-app.sh` | `platforms/pocketbook/spike/` |
| `../src` (upstream export, later a checkout) | this repository |
| `build/kobalt-pb.tar.gz` | `build/pocketbook/kobalt-pb.tar.gz` |

The one-off `tools/patch-*.py` scripts mentioned below applied changes to the host and plugin
that are long since part of the sources; they were not kept.

---

# pbqt — Qt 5 on PocketBook through inkview (spike)

Feasibility spike for a Kobalt port to PocketBook: a static Qt 5.15 built with the
PocketBook SDK, a Qt platform plugin (`pbink`) that draws into inkview's task framebuffer
and pumps inkview's event loop, and a small Qt Widgets app to try it on the Era Lite.

## Layout

- `tools/Dockerfile` — build image (Debian bookworm + the SDK's missing `libmpfr.so.4` link).
- `tools/build-qt.sh` — configures and builds static qtbase 5.15.19 for the B288 toolchain.
- `tools/build-app.sh`, `tools/remote-build.sh` — build the plugin and the app on the docker VM.
- `plugin/` — the `pbink` QPA plugin (static): `PbInkScreen` (QFbScreen → inkview framebuffer +
  `PartialUpdate`/`DynamicUpdate`/`FullUpdate`), `PbInkEvents` (`PrepareForLoop` +
  `ProcessEventLoop` from a Qt timer; pointer → mouse events, keys → Qt keys).
- `hello/` — the test app; imports the plugin statically.
- `device/pbqt-hello.app` — launcher script for `/mnt/ext1/applications/`.

## Build

On the docker VM, `~/build/pbqt/` holds `sdk/` (pocketbook/SDK_6.3.0 branch 6.5),
the qtbase source, `qt-build/` and the installed `qt-pb/`.

```sh
docker build -t pbqt-build -f tools/Dockerfile .
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$HOME/build/pbqt:/work" -w /work pbqt-build bash /work/build-qt.sh
tools/remote-build.sh docker     # from this folder: plugin + app, binary lands in build/
```

## Install on the device

Copy `build/pbqt-hello` to `applications/pbqt/pbqt-hello`, `device/pbqt-hello.app` to
`applications/pbqt-hello.app`, and a few TTFs into `applications/pbqt/fonts/`. The app shows
under Apps → User's. Log: `applications/pbqt/log.txt`.

## Result (2026-10-27, Era Lite PB710, firmware U710.6.11.2124)

The first build ran on the device without changes: 1264x1680 at 300 dpi (8-bit task
framebuffer from inkview), Fusion-styled widgets drawn correctly, taps delivered as mouse
events (button, combo box with its popup, line edit with blinking cursor, checkbox, slider),
all three refresh modes work, "Fast refresh" (DynamicUpdate) looked best for UI interaction,
full refresh and quit clean. Binary: 13 MB stripped, dynamically linked only to libinkview
and the C library; no root needed.

What this proves for a Kobalt port: Qt Widgets + a QFbScreen-based platform plugin on inkview
is a workable base; the unknown was the screen and input path, and it is solved in ~400 lines.

Second run (event logging on): the physical keys arrive as EVT_KEYPRESS/EVT_KEYRELEASE with
the IV_KEY code in par1 (NEXT 25, PREV 24, MENU 23, HOME 26) and long presses as EVT_KEYREPEAT
with the repeat count in par2; page buttons mapped to PageUp/PageDown drive the counter and, as
Qt keys do, the open combo popup; Home quit the app through the plugin's Key_Home mapping (the
firmware's own Home action did not pre-empt it). inkview also sends EVT_MTSYNC (39) for every
touch frame and EVT_POINTERHOLD (35) for a long touch; both are ignored for now.

Open points for a real port: multitouch (EVT_MTSYNC/GetTouchInfoI), orientation, suspend
(inkview's sleepmode is 1; standby behaviour untested), frontlight (SetFrontlightState),
on-screen keyboard (inkview's or a Qt one), OpenSSL for Network, a CMake toolchain file,
C++17 on gcc 6.3 (Kobalt needs C++17; `-std=c++1z` is partial on 6.3), and the CPU cost of
pumping ProcessEventLoop every 20 ms (raw evdev input like KOReader's raw mode would avoid it).

## Kobalt compile check (2026-10-27): can Kobalt's code be built for PocketBook?

`kobalt-check/` configures Kobalt's own `dependencies/` recipes and compiles everything
under `src/` (the portable core, the reader UI and the standalone library) against the static Qt,
without linking. Two toolchains, run with `tools/kobalt-check.sh`:

**SDK gcc 6.3 (`tools/pocketbook-sdk.toolchain.cmake`): not viable.** gcc 6.3 has no inline
variables, structured bindings, `if constexpr`, `<optional>`/`<variant>`/`<string_view>`
(`tools/cxx17-probe.sh`). Kobalt's headers use 203 namespace-scope `inline constexpr`
constants and two structured bindings; KOReader's CREngine fork uses `if constexpr` in
`lvhashtable.h`. Result: CREngine does not build, and of Kobalt's sources only 22 objects
compiled, 52 files failed, 14 000 error lines, nearly all "declared as an inline variable".
The other third-party libraries (FreeType, HarfBuzz, FriBidi, utf8proc, libjpeg-turbo, xxHash,
libunibreak, lunasvg) do build with gcc 6.3.

**Debian's GCC 12 for arm-linux-gnueabi + the SDK sysroot (`tools/pocketbook-gcc12.toolchain.cmake`):
the way out, and the same trick kobuild uses for Kobo** (Ubuntu's GCC 11 + NickelTC's sysroot).
What it took: `-B`/`-L` so the link uses the sysroot's crt and glibc 2.23 instead of Debian's
2.36 (a C++17 test program then needs only `GLIBC_2.4`), `-nostdinc` plus an explicit
`-isystem` list so the sysroot's C headers replace Debian's while GCC 12's libstdc++ headers
stay in front, static libstdc++/libgcc, softfp ARMv7 flags, `-no-pie`, and an overlay copy of
libstdc++'s `c++config.h` (`tools/make-cxxconfig-overlay.sh`) with the glibc-2.3x-only
features switched off (`pthread_*_clocklock`, `getentropy`, `arc4random`), because Debian's
libstdc++ was configured against glibc 2.36. A toolchain built against the sysroot
(crosstool-ng, as koxtoolchain does) would not need the overlay and is the cleaner long-term answer.

Two more things the check needed that a port needs too: the static Qt had to be rebuilt with
accessibility (Kobalt's widgets call `setAccessibleName`) and Qt Sensors 5.15.19 built on top of
it (`tools/build-qtsensors.sh`, for `reader_orientation.cpp`); and Kobalt's `dependencies/`
recipe needs a PocketBook case next to Android to build libpng 1.6, because the SDK sysroot's
libpng is 1.2.59 and its header breaks CREngine (`pngconf.h` setjmp check).

**Result with GCC 12 (clean configure): everything compiles.** All third-party libraries
including CREngine built, and all 89 source files under `src/` (97 compile units with moc and
resources) compiled with zero errors and zero warnings, with the full C++17 code unchanged.
The remaining steps towards a running Kobalt are therefore engineering, not porting of the
language level: a `platforms/pocketbook` target (host services, inkview adapters, the pbink
plugin), the link (watch for glibc symbol versions above 2.23 pulled in by libstdc++ 12's
archive, e.g. `getentropy`), OpenSSL for Qt Network, and the device work listed above.

## Kobalt for PocketBook (2026-10-27/28): `kobalt-pocketbook/`

A first standalone target: Kobalt's reader (`src/` without the standalone library), the
`ReaderController` of the Android target (one Qt 5 fix: `QString::back()` is a `QCharRef`),
the pbink plugin and a small host (`host.cpp`: opening view, then the reader; Home/Back closes
the book and quits, which returns the PocketBook to its library). Services (`services.cpp`):
battery and frontlight/warmth through inkview, page buttons, no on-screen keyboard, no
dictionaries. `display.cpp` maps a "clean" page refresh to a full e-ink update.

Build chain (all GCC 12 + SDK sysroot, see `tools/`):

1. `kobalt-check` once, to build Kobalt's third-party libraries; copied to `/work/kobalt-deps`
   (`install/`, `crengine-src/`, `crengine-build/`) so later builds do not depend on the
   dependency hash directory.
2. `tools/build-qt-gcc12.sh`: static qtbase + qtsensors with the qmake device spec
   `tools/mkspec/linux-pocketbook-g++`, built on Kobalt's FreeType/HarfBuzz/libpng/libjpeg
   (named by archive path, or the sysroot's old shared FreeType wins) so Qt and CREngine share
   one copy of each. Installed to `/work/qt-pb12`.
3. `tools/remote-build-kobalt.sh`: CMake project `kobalt-pocketbook/`, package
   `build/kobalt-pb.tar.gz`; `tools/install-kobalt.sh /l` puts it on the device and adds
   `kobalt.app` to the EPUB handlers in `system/config/extensions.cfg` (KOReader stays default).

GCC 12 + glibc 2.23 lessons beyond the compile check: `-static-libstdc++` must find GCC 12's
`libstdc++.a` before the sysroot's 6.3 one (`-L<gcc dir>` first, else
`std::__throw_bad_array_new_length` is missing); libstdc++ 12 was built for glibc >= 2.34, so
it needs `-pthread` explicitly and references `__libc_single_threaded`, `getentropy` and
`arc4random`, supplied by `tools/glibc-compat.c` (`/work/gcc12-compat/glibc_compat.o`, linked
into everything). A gcc-6.3-built Qt cannot be linked by GCC 12 (`std::__atomic_futex_unsigned_base`
signature changed), hence the Qt rebuild. `nickel4::configureLoopback` already exists in
`src/publication/loopback.cpp`; CREngine's `FT_Load_Glyph`/`FT_Render_Glyph` are renamed with
objcopy to Kobalt's wrappers as on Kobo/Android.

### First runs on the Era Lite (2026-10-27)

Kobalt's reader works on the device: Neuromancer opened from Apps (via `open.txt`), fonts,
justification, margins, frontlight brightness and warmth and dark mode all behaved. Fixes after
the first runs:

- **Window size**: Kobalt's font setup creates a hidden window first, so QFbScreen did not force
  the host full screen; the host now sets its geometry to the panel itself.
- **"Open with" never launched the app**: the firmware runs file handlers from
  `system/bin/<name>.app`, not from `applications/` (KOReader's `system/bin/koreader.app` is a
  two-line forwarder to its real launcher). The installer now puts the launcher there as well.
- **Page buttons**: the reader handles page keys only while it is the active modal widget (as on
  Kobo); in this host it is a child widget, so the host forwards PageUp/PageDown to
  `ReaderWindow::pageButton()`.
- **User fonts**: drop `.ttf`/`.otf`/`.ttc` files into `applications/kobalt/fonts/` (flat).
  The PocketBook's own `system/fonts` is not scanned because Kobalt reads one directory, not
  its subfolders.
- **Dictionaries**: Kobalt uses StarDict dictionaries from its own catalogue
  (`assets/dictionaries.json`, zips on github.com/DanielGregorini/koreader-dicts). This build has
  no TLS, so downloads inside the app fail; instead download `<id>.zip` (e.g. `en-en.zip`,
  `es-en.zip`) on a computer, put it in `applications/kobalt/dictionaries/`, and the app installs
  it on the next start into `profile/config/dictionaries/<id>/` (zip renamed to `.installed`).

### Second round of device fixes (2026-10-27, evening)

- Frontlight went to 0 on exit: Kobalt's light panel calls `finishGesture(false)` on close
  without a gesture; the controller restored an unset "original". Now records the level at
  `initialize()` and restores only after a real gesture.
- Power button: the firmware delivers a short power press as `IV_KEY_MENU` (code 23) when set to
  "menu"; the host maps power/Menu keys to a centre tap on the reading page, which opens
  Kobalt's menu.
- Rotation: inkview sends `EVT_ORIENTATION` with 0 upright, 2 clockwise, 3 upside down,
  1 counter-clockwise (KOReader's decoding). `orientation_sensor.cpp` registers a Qt Sensors
  orientation backend (`pbink.orientation`) fed by the plugin's `deviceOrientation(int)` signal,
  so Kobalt sees a real sensor (auto-rotate option appears). Kobalt's `turn` callback reaches
  `pbinkSetOrientation`: the plugin rotates the composed frame on the portrait panel
  (Landscape = 270° clockwise, InvertedPortrait = 180°, InvertedLandscape = 90°, Qt's own
  `angleBetween` convention), maps touches back, resizes the QFbScreen geometry and reports the
  orientation to QScreen with the update mask widened.

### TLS and orientation polish (2026-10-27, night)

- OpenSSL 3.5.9 built static for the sysroot (`tools/build-openssl.sh`, `/work/openssl-pb`), Qt
  reconfigured `-openssl-linked` with the archives named by path; the package carries Mozilla's
  `cacert.pem` (fetched to `/work/cacert.pem` on the VM) and the launcher sets `SSL_CERT_FILE`
  to it, since Qt finds no root store on the firmware. `downloadableDictionaries` is on, so
  Kobalt's dictionary page can download inside the app; the offline zip route stays.
- Startup orientation: the firmware starts a task in the orientation the device is held in and
  keeps it, so the plugin pins inkview's task orientation to portrait at start and on
  EVT_SHOW/EVT_FOREGROUND (`lockPortrait()`). Before a book is open the host rotates from the
  gravity sensor (debounced 250 ms); Kobalt's sensor backend starts from the last gravity event
  the plugin saw, because `GetGSensorOrientation()` can lag and flipped the page at open.
- inkview orientation codes (header): 0 portrait, 1 landscape 90, 2 landscape 270, 3 portrait 180.
- Full-screen refresh: Kobalt requests a flashing refresh (page count / chapter in "smart"
  mode, menu open, annotation mode) by setting the dynamic property `reader_full_flash` on its
  window; the platform must consume it. The host watches `QDynamicPropertyChangeEvent` for it,
  clears it and asks the plugin to flash the next composed frame (`pbinkFlashNext`).
- Closing in the other orientation flashed the opening page: Kobalt restores the start
  orientation in `ReaderOrientation::stop()`. The host stops painting on `retiring` and ignores
  rotation requests once quitting.
- Dictionaries: only Kobalt's catalogue entries are recognised (checksummed zips, listed from the
  catalogue); folders of other StarDict dictionaries are not. In-app download works with TLS.

### Status at the end of 2026-10-27

Working on the Era Lite: opening from the library (Open with / tap once chosen), Kobalt's
reader with fonts, margins, justification, dark mode, frontlight brightness and warmth,
page buttons (swap with inversion), power/Menu button opens the menu, auto-rotation from the
gravity sensor including upside-down start, smart-mode and menu full refreshes, strip page-turn
animation (plugin-style), user fonts from `applications/kobalt/fonts`, catalogue dictionaries
by in-app download (TLS, Wi-Fi must be on in the firmware: inkview `QueryNetwork()` 3 = off,
515 = connected) or by zip drop, clean exit back to the library. Startup takes ~5 s of font
registration before anything appears, then ~10 s to open a book the first time.

Not done: on-screen keyboard (search cannot be typed), landscape not tested, USB-connection
handling verified only as far as the prompt and the export (the plug arrives as
EVT_BACKGROUND; what closes Kobalt afterwards is not logged, the log lives on the exported
storage), the 20 ms inkview event pump's CPU cost, packaging as a proper PocketBook app
(icon, version), and upstreaming as `platforms/pocketbook` in Kobalt. (Library progress and
PBReader position came on 2026-10-28, see below.)

### Housekeeping (2026-10-27, late)

- The PocketBook library indexes every `*.txt` as a book: logs are now `kobalt.log` /
  `pbqt.log`, the test file is `open.cfg`, and the app deletes the dictionaries' `README.txt`
  at start (`tools/pocketbook-desktop.py` cleans existing ones).
- Home screen: `tools/make-icons.py` draws `device/icons/desktop_app_kobalt{,_f}.bmp` (8-bit BMP,
  106x110: bars glyph black on white; tapped: white on a black disc);
  `tools/pocketbook-desktop.py <mount> [--reset-handlers]` copies them, adds `@kobalt=Kobalt` to
  `system/language/en.txt`, registers `U_kobalt` in `system/config/desktop/view.json` (inside
  `applications`, plus the `@General` group; backup `view.json.before-kobalt`) and, with the
  flag, points `handlers.cfg` entries back at `koreader.app`. The installer runs it
  (`KOBALT_RESET_HANDLERS=1` for the flag). KOReader stays the default EPUB handler; Kobalt is
  an "Open with" choice.

### Warmth in 1 % steps (2026-10-28)

Kobalt's light panel builds the warmth slider with a fixed 10 % step and snapping stops (Kobo's
ten warmth levels) and rounds the shown value to tens, whatever the host reports. The build now
applies `tools/kobalt-patches.py` to the Kobalt tree before configuring: the slider step and the
rounding follow `LightControls::warmthStep()` relative to the warmth range (1 % on the
PocketBook, still 10 % on a ten-level host). The same script also carries the libpng one-liner,
so a fresh Kobalt export needs no manual edits. Both are candidates for upstream.

### Loading-screen stall (2026-10-28)

Twice, on a quick reopen after closing, a book stayed on the loading screen until the device was
put to standby and woken. Kobalt's diagnostics showed the engine's opening job starting ~80 s
late (its own work still ~5.5 s), resuming exactly when the power button was pressed; the engine
fetches resources from a server on the GUI thread, so a blocked GUI thread stalls it. The event
pump was not the cause (it logs calls > 200 ms, none during the stall; `ProcessEventLoopQuick`
is used now anyway). Suspected: an inkview update call blocking in the display driver while the
previous task's last update was still in flight. Mitigation: `WaitForUpdateComplete()` right
after `OpenScreen()` before the first paint, and every update call is timed (warning > 300 ms,
with its name). After that, repeated quick reopens worked every time; not seen again so far.
The warmth gesture snaps to 1 % now as well (patch 3 in `tools/kobalt-patches.py`).

Root cause found (2026-10-28, later): Kobalt's `open-end` reports `elapsed_ms` from a monotonic
timer (5.5 s) while the UTC stamps of the same job span 78 s: the device was suspended in the
middle of the opening. inkview's sleep mode 1 puts the device into light sleep a few seconds
after the last input, regardless of work in progress, and loading has no input; a tap or the
power button wakes it, which is why those "fixed" it. The host now calls `iv_sleepmode(0)`
(`pbinkSetKeepAwake`) from the start of an opening until `opened`/`failed`, then restores mode 1
so idle reading still sleeps as in the stock reader. Not covered yet: long background work after
opening (dictionary downloads, neighbour rendering) can still be paused by a light sleep until
the next touch.

### Library progress and PBReader position (2026-10-28)

Kobalt keeps its progress in its own `profile/config/kobalt.sqlite`; the PocketBook home screen
and library read theirs from the firmware's `system/explorer-3/explorer-3.db` (SQLite, WAL,
firmware-owned), and PBReader keeps its place in the same row. So a book read in Kobalt showed
stale progress, and PBReader opened it at the cover. The port now gives that database the role
Nickel's database has on Kobo, through Kobalt's own native-progress machinery rather than host
hooks:

- `kobalt-pocketbook/explorer_progress.cpp` is a `ReadingBridge` over `books_settings`: the
  book's row is found through `folders.name` (the directory, no trailing slash) and
  `files.filename`, the profile through inkview's `GetCurrentProfile()` (falls back to 1).
  `read()` returns PBReader's `position` as the location, `position_ts` as the modification
  time and `completed` as the status; `write()` checks the row is still the one the reader last
  saw (else Kobalt offers the newer position as a choice, as on Kobo), then UPSERTs `cpage`
  (0-based), `npage`, `completed` (a finish accepted in Kobalt's finish dialog), `opentime` and,
  when the position mapped, `position` + `position_ts`; a favourite mark survives. The database
  is copied once to `explorer-3.db.before-kobalt` (`VACUUM INTO`) before the first write. A book
  the library does not list gets no bridge and saves locally only. The bridge also keeps
  Kobalt's local reading-speed record going, since the firmware has no pace estimator.
- `kobalt-pocketbook/epub_cfi.cpp` is the `PositionCodec`: PBReader stores an EPUB CFI with its
  own prefix, `pbr:/webkit?##epubcfi(/6/22!/4/2/174/1:52)` (spine step from the OPF, spine item
  `2 * (index + 1)`, the chapter's `<body>`, then the element path and a text offset). CREngine
  keeps one `DocFragment` per spine item, numbered `_doc_fragment_<index>`, so encode walks from
  the page's anchor up to the chapter body counting source elements (CREngine's
  autoBoxing/floatBox/inlineBox wrappers are transparent, generated pseudo elements dropped);
  decode walks down the same way. Whitespace is normalised by the engine, so a text offset is
  approximate; the element (paragraph) is exact for ordinary XHTML. Kobalt patch 6 gives the
  codec the same friend access to the engine document as Nickel's codec.
- `reader_controller.cpp` seeds an opening like Kobo's reader coordinator: it reads the row
  before opening, opens at PBReader's position when it is newer than the one Kobalt last saw
  (`preferNativeProgress`) and hands the bridge and both baselines to `ReadingBook`. From there
  Kobalt's reader does the rest: it writes at open (`recordOpening`), at every standby and USB
  plug (`sleepReader`) and at close, polls the row every minute and shows its "synced position"
  dialog when PBReader moved the book meanwhile. Writes are logged
  (`library progress written: book 497 profile 1 page 143/496, position pbr:/webkit?##epubcfi(/6/14!/4/2/36/1:0) in 20 ms`);
  no refresh event is sent, the library picks the row up by itself.

The dialog strings are Kobalt's Kobo ones ("Your device just finished syncing...", "Nickel
progress changed"): PBReader's position plays the part of a sync. The static QSQLITE driver was
already in the binary (Qt 5.15's CMake config imports static plugins of linked modules).

### What a PocketBook target needed that Kobo did not (summary, 2026-10-28)

Platform/plugin (`pbink`): a Qt platform plugin on inkview (task framebuffer + Partial/Dynamic/
FullUpdate, event pump from a Qt timer, pointer → mouse, IV keys → Qt keys); task orientation
pinned to portrait at start/show/foreground, logical rotation with touch mapping, Qt orientation
reporting; first frame after EVT_FOREGROUND; a Qt Sensors backend on EVT_ORIENTATION;
keep-awake (`iv_sleepmode(0)`) while opening a book or downloading; strip-wipe page animation
instead of the Kobo hwtcon sweep; startup `WaitForUpdateComplete`; a frame that follows the
previous update by less than 350 ms (450 until 2026-10-29) is held back until then (`PBINK_MIN_FRAME_MS`;
`WaitForUpdateComplete()` returns at once on this firmware, 0 ms in every log line) and from the
fifth frame in a row within 0.7 s of the previous one the wipe is dropped (`PBINK_ANIM_BURST`,
default 4 since 2026-10-29, 3 before: at 450 ms pacing the controller absorbed three wipes before its queue fell behind), since the controller was still executing the previous
wipe's strips when the next page was blitted under them (tearing when turning quickly). The
sleep-image composition after a turn cost 140-165 ms on the GUI thread, mostly scaling the cover:
Kobalt patch 7 caches the scaled cover, and a turn within a second of the previous one postpones
the refresh to 3 s after the burst ends.
Host: full-screen host window (Kobalt's font setup creates a hidden window first); page keys
forwarded (reader handles them only as the active modal widget); power/Menu → centre tap;
sleep screen: Kobalt's sleep image (cover or page, progress, battery) written as the firmware's
cached lock bitmap (`system/resources/Line/taskmgr_lock_background{,_landscape}.bmp`, 8-bit
grey) and reloaded by `SendEventTo taskmgr.app 154` while awake, 0.3 s after the last page
paint (page turn, TOC jump) and at book open (Kobalt patch 5 exposes `composeSleepImage()`),
with the device held awake (`iv_sleepmode(0)`) from the page turn until the write is done,
because the Era Lite light-sleeps within seconds of a button press and a suspended CPU runs no
Qt timers; identical images are skipped, the write runs on a worker thread (0.1-0.2 s); the
image is turned into the panel's orientation (the task is pinned to portrait and the firmware
draws the bitmap unrotated, so an inverted or landscape reader would otherwise get an upright
lock screen) and written to the portrait file only (the skip of an unchanged image counts the
rotation: a page reads the same upside down, and the turn alone left the firmware the old
bitmap until the wake reload, 2026-10-29); the reflowed page after a screen turn is
flushed with one full flash (host: the first page paint after the turn at which `readyForPresentation()` holds, since the page also repaints under the layout notice 1 s into the reflow; Kobo's
panel flashes on a turn by itself, this one would keep the layout notice's ghost. Kobalt's own
`resizePending` path cannot serve here: `Reader::resizeEvent()` arms it only when the reader
owns a window handle, and in this host it is a child of the stacked widget); the page under a
closed KOsync dialog (opening notice, position choice, manual-sync prompt) gets the same treatment,
since Kobalt hides those without a flash request and the card ghosted (2026-10-29); at EVT_BACKGROUND Kobalt's own sleep sequence
runs and a reload is deferred to the wake, since the firmware draws the lock screen from its
cache before the app hears of the standby and a reload during the transition leaves the device
half asleep; the same happens to a lock pressed while taskmgr reloads the bitmap after a page
turn (seen twice: reload requested 3-4 ms before EVT_BACKGROUND, 50 s to wake, frontlight stuck
off, lock screen shifted). Nothing the app does refuses a lock (`iv_sleepmode(0)` does not, seen),
so the refresh runs 0.3 s after the page button press, when the hand is still on that button and
a lock is least likely; write plus reload are over within about a second of the turn. The window
is narrowed, not closed. A USB plug also arrives as EVT_BACKGROUND (the PC Link / Charge prompt takes the
foreground; no EVT_USBSTORE_IN reaches the task), so the same sequence saves progress and ends
with `sync(2)`: the PC Link export was seen to drop everything the app had written since the
last sync (a standby's suspend syncs; `fdatasync` on the log did not survive), so the host syncs
at every EVT_BACKGROUND and at wake. PC Link then closes Kobalt when the storage is exported, as
it closes KOReader. Residual risk: a lock
double-press started within ~1 s of a page turn can still catch the reload;
native progress: the firmware's `explorer-3.db` row as Kobalt's native reading progress (a
`ReadingBridge` plus an EPUB CFI `PositionCodec` for PBReader's `position`), so the home screen's
progress follows Kobalt and PBReader opens where Kobalt left off, with Kobalt's own open/standby/
close saves and its conflict choice, as Nickel's database on Kobo;
`reader_full_flash` / `reader_animation_direction` / `reader_exit_clean_pending` properties
consumed; orientation callback; no painting while closing; light controller with
`initialize()`-time original and gesture-only restore; page-button pair swap when inverted.
Firmware integration: launcher scripts in `applications/` and `system/bin/` (handlers run from
the latter), `extensions.cfg` + `handlers.cfg`, `GLIBC_TUNABLES` execstack workaround,
`view.json` + `system/language/en.txt` + 8-bit BMP icons for name and icon, no `*.txt` in the
app folders (library indexes them), `cacert.pem` + `SSL_CERT_DIR`, Wi-Fi must be up.
Toolchain: GCC 12 + SDK sysroot (libstdc++ order, `-pthread`, glibc shim), Qt on Kobalt's own
FreeType/HarfBuzz/png/jpeg + OpenSSL, Qt Sensors, libpng 1.6 instead of the sysroot's 1.2.
Kobalt source patches (`tools/kobalt-patches.py`): libpng case for PocketBook; warmth slider and
gesture step from `warmthStep()` (Kobo has 10 levels, PocketBook 100); DictionaryManager
parented to the reader; `composeSleepImage()` factored out of `sleepReader()`;
friend access to the engine document for the PocketBook position codec; plus
`QString::back()`→`QChar` in the copied Android reader controller.

### Upstream develop 34e9b58 (2026-10-29): KOsync, Kobalt's keyboard, CREngine upgrade

The port moved from the beta 4 export to the `develop` branch (71 commits: KOsync progress sync,
a shared on-screen keyboard, reading history and position saves on a profile database writer
thread, a CREngine upgrade with three new patches, header/footer sizes, Publisher Fonts as
default, dictionary lookup editing, annotation export, sleep-screen tile styles, ...).

How the tree is kept now:

- `~/build/pbqt/kobalt` on the docker VM is a git checkout of nicoverbruggen/kobalt (the beta 4
  export is kept as `kobalt-beta4`). To move to another commit: `git -C ~/build/pbqt/kobalt
  checkout -- . && git fetch origin && git checkout <commit>`; `tools/kobalt-patches.py` is applied
  again at every build. Check the anchors on the PC first: `git -C ../src fetch origin
  '+refs/heads/*:refs/remotes/origin/*'`, `git -C ../src worktree add --detach ../src-develop
  origin/develop`, `python3 tools/kobalt-patches.py "$PWD/../src-develop"` (then
  `git -C ../src-develop checkout -- .`).
- When `dependencies/manifest.json` or `dependencies/patches/` moved: `tools/remote-rebuild-deps.sh
  docker` rebuilds the third-party libraries in `kobalt-check-gcc12` (only what changed) and
  refreshes `kobalt-deps/` (CREngine source and build; the install prefix only if it differs, in
  which case Qt needs `tools/build-qt-gcc12.sh` again). Then `ssh docker rm -rf
  ~/build/pbqt/kobalt-pb-build` for a fresh configure and `tools/remote-build-kobalt.sh docker`.
- `KOBALT_COMMIT` in `kobalt-pocketbook/CMakeLists.txt` names the upstream commit (About screen).

What the port needed for develop:

- `reader_controller.cpp` (the hand-copied Android controller) is gone: patch 8 in
  `tools/kobalt-patches.py` adds the PocketBook hooks to `platforms/android/reader.cpp` in place
  (orientation sensor, library row as native progress with the EPUB CFI codec, the Qt 5 `QCharRef`
  spelling) and CMake compiles that file. Upstream changes to the controller come for free.
- Patch 5 is rebuilt on upstream's new `captureSleepScreen()` (the sleep image is now a
  `sleep::Snapshot` backdrop plus a status tile in one of three styles); `composeSleepImage()`
  composes backdrop and tile with the current battery, the host's lock-bitmap refresh is unchanged.
  Patch 7's cover cache sits in `captureSleepScreen()` now, so `sleepReader()` profits too.
- Keyboard: `result.keyboard = ui::makeWidgetKeyboard`, Kobalt's own three-row keyboard (search,
  dictionary lookup editing, KOsync account fields). The "no on-screen keyboard" open point is closed.
- KOsync: `ReaderHostServices::syncNetwork` is a `SyncNetwork` on inkview (`services.cpp`):
  `connected()` = `QueryNetwork() & NET_CONNECTED`; `prepare()` runs `pbinkConnectNetwork` from the
  event loop (interactive: `WiFiPower(1)` + `NetConnect`, with the firmware's network dialog if it
  has to ask; silent, for the checks at book open and wake: `NetConnectSilent`, known networks only).
  `observeWifi` polls `QueryNetwork()` every 5 s for the menu's Wi-Fi indicator, with the link
  quality from `/proc/net/wireless`. `deviceName` comes from inkview's `GetDeviceModel()`
  ("PocketBook ..." + " (Kobalt)" is KOsync's default device name). At standby the host keeps the
  device awake while `pendingNetworkSync()` (the reader's bounded final upload, 2 s), 3 s at most.
- `annotationExportDirectory` = `/mnt/ext1/Kobalt/Annotations` (without it Kobalt opens a file
  dialog); the library will list the exported HTML.
- CMake: `src/scenario/` excluded as on Kobo (evaluation packages only), `reading_timer.cpp` built
  with `KOBALT_READING_CLOCK_BOOTTIME` (reading time keeps counting through light sleep, as on
  Kobo), `Qt5Widgets_PRIVATE_INCLUDE_DIRS` for `theme.cpp`.
- Two build fixes found by the first develop build: patch 9 spells `values = QStringList{...}`
  in `settings_panel.cpp` (a braced list of string literals assigned to a `QStringList` is
  ambiguous for Qt 5.15; Kobo's Qt 5.2 and Qt 6 accept it), and `kobalt-pocketbook/futex_compat.cpp`
  supplies `std::__atomic_futex_unsigned_base`'s three methods on the futex syscall: Debian's
  arm-linux-gnueabi libstdc++ 12 is built for ARMv5, where atomics are not lock-free, so its
  `futex.o` is empty, while `<future>` compiled for ARMv7 (Kobalt's `std::async` font preparation
  in `fonts.cpp`) expects them; without the shim the linker fell back to the sysroot's shared
  libstdc++ 6.3 ("DSO missing from command line").
- Unchanged and still needed: patches 1-4 and 6 applied to develop without changes; `explorer_progress.cpp`
  and `epub_cfi.cpp` compile against the unchanged `ReadingBridge`/`PositionCodec` interfaces
  (`ReadingBook` gained a trailing `sourceKey`, `OpenRequest` a trailing `syncFilename`, both default).

To check on the device: KOsync sign-in (Settings → Sync, Kobalt's keyboard, Wi-Fi brought up by
the firmware dialog), sync at open/wake (silent reconnect; `kobalt.log` has `pbink: NetConnectSilent
returned ...`), the Wi-Fi indicator in the menu, the lock bitmap with the band/bookmark tile styles,
the library progress and PBReader position after a standby (unchanged code, new upstream save path on a
writer thread), dictionary lookup editing, annotation export. The reading-history profile lock is a
kernel `flock`, released when the process dies (PC Link kills Kobalt), so no stale-lock case.

## Name and icon on the PocketBook home screen (manual firmware files)

These live in the firmware's user config, not in the app package, so a plain copy of the zip
cannot do it; `tools/pocketbook-desktop.py <mount>` does all of it, and this is what it writes
(from jjrrw174's PocketBook-Desktop-and-App-Customizations guide, verified on the Era Lite,
firmware U710.6.11.2124):

1. **Icons** — `applications/icons/desktop_app_kobalt.bmp` (normal) and
   `desktop_app_kobalt_f.bmp` (while tapped): 8-bit BMP, 106 x 110 px on the Era Lite
   (`tools/make-icons.py` draws them).
2. **Display name** — one line in `system/language/en.txt`: `@kobalt=Kobalt`. The firmware lists
   every `applications/*.app` under the translatable key `@<name>`.
3. **Icon mapping** — `system/config/desktop/view.json`: inside the `"applications"` object add
   ```json
   "U_kobalt": {
     "path": "/mnt/ext1/applications/kobalt.app",
     "title": "Kobalt",
     "icon": "/mnt/ext1/applications/icons/desktop_app_kobalt.bmp",
     "focused_icon": "/mnt/ext1/applications/icons/desktop_app_kobalt_f.bmp"
   }
   ```
   and append `"U_kobalt"` to the `apps` list of the `@General` group under `"view"`. The block
   must be inside `"applications"` (a top-level sibling is silently ignored). Keep a backup;
   deleting `view.json` makes the firmware recreate the default one at the next boot.
4. Safely eject and reboot; the name and icon appear in Apps. The same name shows in the
   "Open with" menu for EPUBs.

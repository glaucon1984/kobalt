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
instead of the Kobo hwtcon sweep; startup `WaitForUpdateComplete`; (the pacing and burst rule
that follow were replaced on 2026-11-01, see "Page-turn tuning from KOReader" below) a frame that follows the
previous update by less than 350 ms (450 until 2026-10-29) is held back until then (`PBINK_MIN_FRAME_MS`; since 2026-10-30 only when it covers at least a quarter of the screen, the light pop-up's frames used to be paced too and trailed the finger;
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

### Page-turn tuning from KOReader (2026-11-01)

`swipeanimation.koplugin` v5.4.1 was tuned on this Era Lite after disassembling the device's
`libinkview.so` and `libhwconfig.so` (firmware U710.6.11.2124; the SDK's 6.5 library differs a lot).
The findings replace the two old pacing rules, which only treated the symptom:

- An update that lands on one the panel is still drawing tears. `GetWaveformTimes` gives GC16
  388 ms, so no spot may be redrawn within about 430 ms (350 and 400 tore in KOReader, 430 did not).
- That rule does not explain the 2026-10-29 result here (450 ms pacing, the fourth quick wipe tore):
  each spot was redrawn over 600 ms apart. That run used `PartialUpdate` and 10 strips; KOReader's
  clean bursts use `DynamicUpdate` and 8. `hw_update` queues in a 16-entry ring, so a wipe of 10 may
  matter more than one of 8. Unproven. If long bursts tear, look there first.
- `DynamicUpdate` is not A2 on this firmware. `PartialUpdate` is `UpdateEx` flags 2 (GC16),
  `DynamicUpdate` 0x40002, the same waveform with a flag that was not traced, yet it looks cleaner
  and quicker on the panel. Why is open.
- `WaitForUpdateComplete()` cannot pace anything: `hw_wait_luts_complete` only stores a flag.
- Tried in KOReader, do not retry: GL16 (`PartialUpdateTextPage`) is no faster and leaves a dirty
  trail, DU (`PartialUpdateBW`) is fast but leaves the page grainy.

`PbInkScreen::doRedraw()` now has one rule. A large frame waits `PBINK_MIN_FRAME_MS` (300) when it
is a wipe in the same direction as the wipe before it, and `PBINK_LONG_GAP_MS` (475) otherwise:
after a full refresh, on a change of direction, and for any plain large frame. `mLastSweep` is
cleared by every large frame that is not a wipe, so a wipe after a full refresh takes the long gap
without a separate timestamp. The burst rule (`PBINK_ANIM_BURST`) is gone: its plain fallback landed
on the strips still being drawn and tore in KOReader. Strips default to `DynamicUpdate`
(`PBINK_ANIM_FAST=0` for `PartialUpdate`), 8 strips, 16 ms.

Differences from KOReader's 250 and 430 ms: KOReader sleeps, draws for about 46 ms, then sends the
first strip, while `QFbScreen::doRedraw()` composes first and the strip follows the sleep at once.
The same-direction safety margin is gap + wipe time (+ draw time in KOReader) and must stay above
388 ms, so fewer or faster strips shrink it.

A full refresh with a small dirty rectangle counts as large here: the old log has such frames, and
`FullUpdate()` redraws the panel whatever was touched, so a turn after one would land on it.

A wipe logs `wipe <direction>: composed in N ms, strips took N ms` after its last strip. The line's
time minus the strip time is the first strip, to compare with the `event 29` line of the tap.

Judged on the device the same day with these defaults: page turns clean and much better than before,
a book with images fine. From the log of that run, on turns that were not paced: page button press to
first strip 162 ms median (81 turns), of which 20 ms is composing the frame; touch about 160 ms, on
only two turns. KOReader's 80-85 ms was measured by the plugin from its own events, so the two are
not directly comparable.

### The event pump's 20 ms wait (2026-11-01)

The same evening the turns still felt slower than KOReader's. The wipe was not why: KOReader logged 130-150 ms and Kobalt 148 ms, and the rate limit of quick turns cannot move, since a spot needs about 430 ms between redraws either way (`PBINK_MIN_FRAME_MS` 300 plus the wipe, KOReader's 250 plus its draw plus the wipe). The gap was before the first strip: KOReader about 52 ms from the turn event, Kobalt 153 ms from the key event.

Joining `kobalt.log` with `page-turn-painted` in `profile/diagnostics/events.jsonl` over 97 turns gave medians of 22 ms press to `turnPage()`, 25 ms for the engine's frame (of which the engine logs 5), 33 ms painting, 19 ms composing and 49 ms from there to the first strip. The 22 and the 25 hardly varied, which pointed at a wait and not at work.

From the device's `libinkview.so`: `ProcessEventLoopQuick()` and `ProcessEventLoop()` are the same code on this firmware. Both call `iv_process_event_loop_ex`, which delivers the queued events, runs timers, task messages, the gravity check and auto-sleep, and then waits up to 20 ms on a condition that the input thread signals. The Qt timer called it back to back, so the GUI thread sat in that wait nearly all the time, and each step of a turn waited for it to end. The exported `iv_process_event_loop_quick` delivers the queue and reads one event from the firmware's message queue, which is non-blocking (`PrepareForLoop` drains it with the same call and `hw_nextevent` treats EAGAIN as empty), and it waits for nothing. It is not in the SDK, so it is looked up with `dlsym`.

`PbInkEvents::pump()` calls it first, every 10 ms. For 250 ms after any key or touch event it is all that runs, apart from the firmware's step once a second. For 120 ms after a press, a key repeat or a touch release the firmware's step is skipped outright, since a press takes 70-90 ms to become a frame. Moves do not count towards that, so a long drag cannot starve the firmware. Idle, the step runs as before. If the symbol is missing it logs a warning and behaves as before. The quick call reads the message queue from the GUI thread while inkview's input thread also reads it. `IsAnyEvents()` does the same from an app's thread, so the firmware allows it, but if keys or touches ever arrive out of order or get lost, look here first. A wipe's debug line now also has `copied in N ms`, the conversion into the 8-bit framebuffer.

The first version ran the quick call only inside the 250 ms window. Judged on the device the same night: "feels great", press to first strip 157 ms down to a 90 ms median over 83 turns. About one turn in six still lost 22 ms, mostly a press made while a wipe blocked the thread that then reached the handler through the firmware's step. Calling the quick loop first and adding the 120 ms rule took that to none in 119 turns.

`PbInkScreen::blit()` replaces QPainter for the copy into an 8-bit framebuffer. QPainter stored through a per-pixel path, and a rotated frame was far worse. The result is the same pixel for pixel, with `qGray()`'s weights. Upright and upside down use NEON, which the whole build targets; upside down reverses each group of eight. Sideways frames are scalar, in 64-pixel tiles because each pixel moves a whole panel line. The other panel depths keep QPainter.

| Rotation | Copy before | Copy now | Press to first strip before | Now |
|---|---|---|---|---|
| 0 | 22 ms | 16 ms | 157 ms | 88 ms |
| 180 | 38 ms | 17 ms | 113 ms, before this copy | 90 ms |
| 90 and 270 | 189 ms | 49-59 ms | 283 ms (270 only) | 122-132 ms |

The upright copy only gained 6 ms, and NEON at 8 pixels a step still takes 16 ms for 8.5 MB, so memory is the likely limit there (composing the same bytes takes 20 ms). Sideways frames could use NEON too (8×8 transposed blocks) if they ever matter. The remaining upright stages are 2 ms press to `turnPage()`, 11 ms for the engine's frame, 35 ms painting, 21 ms composing and 16 ms copying, and the painting and composing are full-screen copies through Qt's backing store. Upright turns in the first 15 s after opening a book take 135-160 ms: composing and copying both rise by about half, which looks like startup work competing for the CPU, as seen in KOReader.

From the logs of the final build and the one before it: all four rotations, standby and wake, closing a book. Keys stayed in pairs in every run (122 of each on the last). Three to six `refresh keep-awake released by timeout` lines a run are not new: the rate is the same as before this work, about one in fifty wipes.

Reading the log: a press followed by `refresh 2` is a full refresh, not a wipe, so a script that pairs presses with wipes reports a false stall of about a second there.

### One flash for a half turn (2026-11-02)

`setRotation()` does a full refresh at once, and `rotateScreen()` also armed `flashAfterTurn()` for the reflowed page. A half turn (0 and 180, 90 and 270) keeps the layout, so no reflow came, and the flash landed on the next page paint, which was the reader's next turn: a second flash, and no wipe on that turn. Hands change sides by turning the device, so this happened about as often as the device was turned.

`rotateScreen()` now leaves `rotationClock` alone for a half turn. It is still armed across portrait and landscape, where the second flash clears the old layout, and when the orientation does not change at all, which is the path a book's opening takes. The reader judged the panel on the device: no ghosting from the one flash. In that run's log the half turns (180 to 0, 270 to 90, 90 to 270) had no reflow flash and the next turn was a wipe, while 0 to 270 and 270 to 0 kept theirs.

### Closing a book the library has as finished (2026-11-01)

Since upstream de96c8a, in the port from v0.3.0, a book with `completed = 1` could not be saved to the
library at close: `native-save` failed with "Nickel progress changed before exit" on every try, with no
`native-save-start` and no "NOT written" line. KOSync and the local checkpoint were not affected. On
2026-10-29, before that merge, the same kind of book closed fine.

`ReadingBook::markNativeReading()` replaced the write at open. It calls `ReadingBridge::markReading()`
and then expects status 1, but the bridge kept the default, which writes nothing and returns true. The
row stayed at status 2, `observe()` ignored it because the time had not moved, so nothing was pending
and yet `mayWrite()` was false.

`LibraryBridge::markReading()` now clears `completed` at open, as Nickel's status update does, and
leaves the position, `position_ts` and `opentime` alone. The row is only touched when it is completed.
The finish dialog still offers to mark the book finished again at close. Confirmed on the device the
same day: the book that failed three times closed on the first try.

A library book with no `books_settings` row would read as invalid and fail the same way, since
`mayWrite()` is false for an invalid progress, but opening one through the firmware does not get there.
Book 441 had no row in the library backup taken before this fix was installed, read as `page 0/0, position none (ts 0)` at open, and saved at close with
`native-save-start` and `verified`. That save needs a valid row from the opening on, so the firmware must
add an empty row when it launches Kobalt. Inferred from the log, as the row was not read during launch.

### A power-off screen of its own (2026-11-02)

The power-off file (`system/logo/bookcover`) used to be a copy of the lock bitmap, so a device
that had switched itself off looked exactly like one on standby. On Kobo the power-off screen is
the same backdrop composed with the "Powered off" status (light paper, no battery), and the same
now happens here: `composeSleepImage()` composes the power-off image from the page capture it
already has (one copy of the frame, a tile paint), the lock-image writer keeps it as an 8-bit
panel-sized image (2 MB) and writes it at the old pace (first lock image of the book, then at
most every 30 s), and the standby sequence writes a fresh one through `composePowerOffImage()`,
which reuses the sleep image just presented (`powerScreenSnapshot()`, as the Kobo host does), so
the standby adds a tile paint and a 0.1 s write, not another capture. A reader that gives no
power-off image (stock sleep screen) leaves the old copy of the lock bitmap. Logged as
`power-off image written (standby)`; `(standby, pending)` is the fallback path.

### The boot capture is tied to the boot (2026-11-02)

Seen in the log: a PC plug at 12:45:53 was taken for a standby (`IsUSBconnected()` was still 0
when EVT_BACKGROUND arrived, or the USB state had not changed since the last key), the capture
stayed valid, and the book opened by hand after the eject showed the captured page instead of
the loading screen; a plug at 13:14:59 was recognised. The USB check is a proxy, as the earlier
three-minute uptime rule was; the fact wanted is whether the device was powered off since the
capture. `bootlogo.book` now carries a third line, the kernel's `/proc/sys/kernel/random/boot_id`
(new at every power-on, unchanged through standby), and `savedBootLogo()` returns nothing when
it matches the current one, logging `boot capture is from this boot`. The USB check stays as an
early forget. A two-line file from an older build is used as before.

### A quiet timed power-off (2026-11-02)

Diego: the automatic power-off after a standby lit the screen for a few seconds instead of
switching off silently. From `kobalt.log` (two cases, 13:56 and 15:45, 30 min after the
standby): the firmware wakes the device, Kobalt gets EVT_UPDATE + EVT_FOREGROUND as for any
wake (so it repainted the page with a flash and started the silent KOsync connection), then
`(iv_poweroff_timer)` is printed by Kobalt's own process and EVT_BACKGROUND follows ~1.6 s
after the wake; the app is then killed. From the device's `libinkview.so`: the timed power-off
is a hard timer ("POWEROFF", `SetHardTimer`, an RTC alarm) that libinkview arms in the
foreground app's process (`SetAutoPowerOff`, `iv_rise_poweroff_timer`, at every input) with
`ivstate.powerofftm - (hw_boottime() - *(ivmpc)+0x16c)` (the last activity shared with the
monitor); `iv_poweroff_timer` fires it unless a player is running, and calls `hw_say_poweroff`.
`ivstate` and `ivmpc` are exported; the offsets (0x68 timeout in **milliseconds**, 0x90 enable
byte, 0x16c last activity in seconds) are 6.11's. The frontlight is the monitor's: `hw_set_frontlight_enabled`
flips the `FL_MODE_ENABLED` bit in the shared state and sends `MSG_SETFRONTLIGHT` (only when the
bit changes); `hw_set_frontlight_state` stores the level and always sends it. Who lights the
panel at resume is in the monitor binary, not read; the fix is empirical.

Fix (`firmware_power.cpp`, `Host::quietForeground`): at the end of the standby sequence Kobalt
switches the light off (`SetFrontlightEnabled(0)`), and the plugin asks the host before the
foreground repaint; with the power-off timer due within 15 s the host keeps the task asleep
(no repaint, no reader wake, light off again if the resume lit it), holds the device awake, and
the power-off's EVT_BACKGROUND confirms it (`timed power-off follows the wake`); otherwise a
press or a 4 s fallback does the normal wake with a full repaint. A press wakes with the light
restored at once (`wake (press)`). KOReader does nothing here (its PocketBook powerd leaves the
light alone at suspend), so it shows the same lit transition.

First test build (17:20, 10-minute timeout): `wake with the timed power-off due in 599398 s`,
so the wake was taken for a press: the timeout is 600000 **ms**, not seconds (I had read the
×1000 in the timer code on the wrong operand), and the power-off's EVT_BACKGROUND then ran the
standby sequence, which switched the light off right before the shutdown. The firmware keeps
the flag across a power-off, and Kobalt's slider only set the level, so the light could not be
switched on from Kobalt afterwards (the firmware's own panel could). Fixes: milliseconds; the
slider goes through `applyFrontlight` (flag with the level, as KOReader) and reads zero while
the flag is off; the standby darkening sets level 0 and the flag and records the level in
`applications/kobalt/light.standby`, which a wake by a press or Kobalt's next start restores
(`frontlight back on at N (start)`); the detected power-off wake sets level 0 again, since the
flag's message only goes out on a change.

### Brightness schedule (2026-11-02)

The light panel's schedule option only followed the firmware's warmth curve; Diego asked for
the brightness curve (`system/config/frontlight/autobrightness.cfg`, "Time Brightness") too.
Kobalt's light panel already has an automatic-brightness option (Kobo's ambient light sensor,
`LightControls::hasAutomaticBrightness` and friends), which the port now backs with the
firmware's own flag, labelled "Follow PocketBook brightness schedule". From the device's
`libinkview.so`: `hw_is_frontlight_auto_supported()` is 1 with a light sensor, else 2 when
`support_fl_time` in `/ebrmain/config/device.cfg` is set (the by-time schedule), else 0;
`hw_set_frontlight_auto_enabled()` flips `FL_MODE_AUTO_BRIGHTNESS` (bit 0 of the shared light
mode, +0x288) and sends `MSG_SETFRONTLIGHT` on a change; `hw_is_frontlight_auto()` reads the
bit. The three are looked up with `dlsym` like the colour ones (`firmware_power.cpp`). A manual
level (slider or edge swipe) switches the schedule off first, because `SetFrontlightState`
stores a level without applying it while the schedule is on; and switching the schedule on
switches the light on first, since at the slider's zero the firmware's flag is off and the
schedule alone left it off (seen by Diego). The standby darkening switches the
schedule off as well and records it, so the level 0 applies; the restore (press, or the next
start after a power-off) switches it back on, whatever the level was: the curve may stand at 0
at the standby (a schedule may well have dark hours; `-1` in the file is the firmware's "off"),
and the firmware applies its current value once the schedule is on again.

### The lock switches the light off itself; the power-off wake is what needs the record (2026-11-03)

Diego, twice: with the schedule on and the light on, standby → timed power-off → power on left
the light at 0 % with the option still ticked. `kobalt.log` 23:40–00:16: the standby line has
no `frontlight off` suffix, i.e. `GetFrontlightEnabled()` was already 0 when the standby
sequence ran (the firmware's lock switches the light off itself, before EVT_BACKGROUND reaches
the app), so nothing was recorded; at the power-off wake the firmware had lit the panel again
(`frontlight on at 74`), Kobalt set level 0 and the flag off, the firmware persisted that, and
with no record the next start restored nothing. So the standby darkening is normally a no-op
and the power-off wake is where the light must be recorded: it now goes through
`darkenForStandby()` too (record + dark), and `standbyLightOff` is set so the fallback wake
restores as well. The restore with the schedule now switches the on/off flag on and the
schedule off and on again: the schedule's flag survives the power-off, and a plain "on" would
send no message, so the firmware would not apply its curve. The earlier manual test (17:20)
had its suffix only because that EVT_BACKGROUND was the power-off's, with the task awake.

### The PIN screen after a timed power-off (2026-11-03)

Diego: the PIN screen came up dark after a timed power-off, the light only came on while
entering the PIN. The firmware saves the light state at shutdown and brings it back at boot,
before the PIN screen, and what it saves is what its monitor last applied: a test build wrote
"on at 73 with the schedule" into the shared light state (`ivmpc->fl`, +0x446c: version, mode
byte, colour, brightness; checked by `hw_set_frontlight_enabled`'s and
`hw_set_frontlight_auto_enabled`'s offsets) right after the power-off's EVT_BACKGROUND, without
the message, and the next boot's shared state read mode 0x02, brightness 0 all the same.
`/mnt/secure/frontlight.dat` (the store path from `device_frontlight_store`) is unreadable to
the app. So "saved on, panel dark" is not available to an app; the light in the PIN screen was
Kobalt's own restore: the firmware launches the last book at about 7 s of uptime, behind the
PIN screen, and `Host::start()` ran 9 s later, after the font setup. The restore now runs in
`main()` right after the QApplication (inkview attached by the plugin, 0.2 s after launch),
with the one in `start()` kept as a fallback. The silent write and the persistence log were
removed again.

### Upstream develop 7415fd2 (2026-11-03): v1.0-beta.6 nightly 1

55 commits past the beta 5 merge: named bookmark shortcuts, an image viewer, inverted greyscale
illustrations in dark mode, reading-time formats, normalization presets, book search from a
lookup, regional keyboard layouts, optional light-gesture feedback, the cover on the opening
screen, layout reuse across viewport changes, a duplicate page-turn filter (200 ms, below the
host's 350 ms held-button repeat), and a CREngine patch (`crengine-alternate-layout.patch`), so
the dependencies were rebuilt (`tools/remote-rebuild-deps.sh`; prefix headers unchanged, Qt
kept). Two conflicts, both in files the fork had touched: the cover decode (upstream dropped
the engine requirement so a cover can be read from a file before the book opens; the fork's
diagnostic stays) and the light gesture, which upstream moved into `dispatchLightGesture()`
behind a posted event (the warmth snap to the host's resolution moved with it). Host changes:
`animationsAvailable` became `animationSupport` (`PageAnimationSupport::Supported`), and the
controller's new `openingCover` signal is shown on the opening view. The `origin` fetch refspec
now also tracks `develop`.

Upstream's light-gesture pop-up now waits for a 150 ms pause in the swipe before it is drawn,
on every platform; that is upstream's design, not a port issue.

### Warmth while the brightness schedule runs (2026-11-03)

Diego: warmth swipes applied only after the finger lifted, until a brightness swipe, after which
warmth followed the finger. A timing diagnostic (one line per hardware write: finger to level,
level to write, call time, CPU clock) showed Kobalt writing the colour every ~120 ms from about
100-280 ms after the press, each `SetFrontlightColor` taking 2-8 ms, in every swipe; the
difference was the brightness schedule, which the brightness swipe had switched off. In
libinkview, `SetFrontlightColor` is `hw_set_frontlight_color`: it stores the colour in the
shared light state (+0x4474, beside the level at +0x4478) and sends the same message (0x11b)
as `hw_set_frontlight_state`, and the monitor applies neither while
`FL_MODE_AUTO_BRIGHTNESS` is set. Not a regression of the merge: it is there since the
schedule option (0.3.6) whenever the schedule is on.

A warmth change now lifts the schedule (`liftBrightnessSchedule()`: flag off, the monitor
re-applies the stored state) and resumes it 1 s after the last warmth change, after the last
colour is written. The shared level is the schedule's current one (the log showed the
schedule's 84 there after a manual 89), so the brightness does not move. While lifted,
`automaticBrightness()` still reports the schedule on, so the panel and the standby record
(`light.standby` "auto") are unchanged; `setAutomaticBrightness()` (a brightness swipe, the
panel option, the standby darkening) ends the lift. Diego: as responsive as on Kobo.

### Upstream develop 898f831 (2026-11-05): v1.0-beta.6 nightly 2

117 commits past nightly 1, 679 files: hanging punctuation (with a CREngine render-overflow
patch and a reworked vertical-justification patch), the page spacing controls in one panel, a
dark mode toggle in the Light panel, the font picker's previews on a worker with a bounded cache
and a 100 MiB font budget, reading progress counted as Nickel does, 100 % on the sleep screen for
finished books, KOSync syncing while reading without a timer and turning Wi-Fi off after a sync
it turned on, an Automatic sync sheet, Unpin bookmark. Most of the diff is Kobo, test and CI
work. The tree was reorganised and reformatted on top: `src/interfaces` became `src/app` and
`src/ui`, the Android host moved to `platforms/standalone` (namespace `kobalt::standalone`),
`ReaderModel` became `EngineDocument`, short namespace aliases went, and every C++ file was
reformatted in the Qt style.

The merge conflicted in 12 files, all ones the fork had touched and upstream had moved and
reformatted; each was resolved by taking upstream's text and putting the fork's change back
(the loading-bar and sleep-image hooks, the warmth step, the PocketBook sleep-screen label, the
cover-decode diagnostics, libpng for PocketBook, the dictionary manager's parent). The light
panel change was redone on the moved file. The port's own sources needed only the new include
paths, the namespace and the class name.

The build system changed more: `dependencies/` is no longer a sub-project but a configure-time
dependency cache (`dependencies/cache.cmake`, `kobalt_dependencies()`), keyed by the toolchain
and the recipes. The port keeps its prebuilt prefix (`KOBALT_DEPS`, which the static Qt was
linked against): `tools/kobalt-check` now calls `kobalt_dependencies()` with the cache at
`/work/dependency-cache` and `tools/rebuild-deps.sh` copies the published set into
`/work/kobalt-deps`. `cache.cmake` learnt `KOBALT_POCKETBOOK` (libpng built into the set as for
Android, instead of `find_package(PNG)`). Prefix headers unchanged, so Qt was kept. The loopback
repair for the resource server moved from `src/publication` to `platforms/kobo/common/loopback.cpp`
(plain Linux ioctls); the port compiles that file. Build clean, no warnings.

Not wired yet: upstream's new host hooks `turnOffWifi` and `holdWifiOff` (the Smart Wi-Fi power
saving, hidden until beta 7) and `kosyncHttp`; the port's `syncNetwork` connection handling stays
as it was.

### Upstream v1.0-beta.6 (2026-11-07): the release tag

25 commits past nightly 2, the beta 6 release (`7ff437e`, main = develop = tag): sleep screens
reworked for beta 6 (a new "PNG over text" mode that draws a transparent PNG of the user's over
the dimmed page, the Bookmark tile as the default, the Band tile gone, a None tile), a light
sleep bookmark in light mode, embedded fonts budgeted smallest first (a CREngine patch,
`crengine-embedded-font-limit.patch`, so the dependencies were rebuilt; prefix headers unchanged,
Qt kept), cached covers shown early, Nickel progress reconciliation fixes, and Kobo test work
(pewter, a fake Nickel). Two conflicts: the strings file (the stock sleep screen's description now
says "Kobo's normal sleep screen", so the PocketBook label got a description of its own, looked up
as the label key plus `_description`) and the sleep screen choice (upstream's tile visibility and
overlay enablement, with the fork's host-named stock label kept). The overlay mode's folder on
PocketBook is `applications/kobalt/screensaver` (`sleepOverlayDirectories`; the installer leaves
it alone), named in a PocketBook description string behind `KOBALT_POCKETBOOK`.

### Upstream v1.0-beta.7 (2026-11-08): the release tag

36 commits past beta 6, the beta 7 release (`e86533c`, main = develop = tag). A clean merge, no
conflicts, and `dependencies/` untouched, so the prebuilt libraries were kept (no deps rebuild;
only the app was built). What it brings: collapsible chapter groups in Contents (a setting under
Chapter List: expand the current section, all, or off), hardware button mapping (Settings →
Controls → Hardware buttons: previous, next or nothing per button), a "disable standby while
reading" option, a live appearance preview while adjusting margins and text (`AppearancePreview`,
the margin preview renamed and widened), the reading link indicator removed and link settings
grouped, About Kobalt reachable from the Library, dictionary size labels that fit the screen,
explained KOSync failures (one string per cause), the Light panel kept below the toolbar, and long
presses no longer turning the page during frame updates. The fork's hunks all survived the
auto-merge (checked with `git diff origin/main` on the touched files).

Port decisions, both in `host/services.cpp`:

- `buttonControls` returns the two page buttons with the keys the pbink plugin delivers
  (`pbinkevents.cpp` maps `IV_KEY_NEXT` to PageDown and `IV_KEY_PREV` to PageUp). On the Era Lite
  the upper button is the firmware's "next" key, so `page_upper` carries PageDown and
  `page_lower` PageUp; the first build had them the other way round, which showed upper =
  previous in the sheet while the button turned forward. The reader compares the pressed key with
  the list (`Reader::pageButton`), so Kobo's `pageButtons::paging()` with F23/F24 would never
  match here. The default direction of each entry is recomputed through `pageButtonDirection`,
  so the upside-down swap still applies to an unmapped button. The ids (`page_upper`,
  `page_lower`) select the e-reader diagram in the mapping sheet.
- The host forwards the page keys itself (the reader only filters them while it is the active
  modal widget). It used to call `pageButton(direction)`, which carries no key and so skipped the
  mapping; it now calls `ReaderWindow::physicalPageButton(key, direction)`, upstream's entry point
  for a hardware key (Android uses it for the volume buttons), for the first press and the held
  repeats alike, so "Do nothing" and a swapped pair work.
- A mapped button stayed with the hardware button when the device was turned upside down (found
  on the device after 0.6.1): the reader matches the pressed key against the list, and a key names
  a hardware button, so once the preference held an explicit action (any visit to the mapping sheet
  stores one) the upside-down swap, which only lives in `pageButtonDirection`'s native direction,
  no longer applied. `buttonControls` now crosses the keys over when the screen is inverted
  (`page_upper` carries PageUp, the hardware "previous" button, which is then on top), so the ids
  name positions as the device is held and the mapped action follows the position. The sheet's
  defaults, recomputed from the same orientation through `pageButtonDirection`, stay consistent
  (inverted PageUp turns next). Upstream's Kobo list uses fixed F23/F24, so an explicit mapping
  there presumably has the same behaviour.
- `readerStandbySupported` stays false, which hides the "Disable standby" option: on Kobo the hook
  lets the reader veto a standby the host is about to request; on PocketBook the automatic standby
  is the firmware's own timer and inkview has no documented call to postpone it. Noted in
  `doc/pocketbook.md`.

Modal description clipped (found on the device, KOSync position choice): beta 7 moved a modal's
title and description into a nested heading row (`PopupCard::heading`), but `fitToWidth` only
fixes the heights of the card's direct child labels and then asks the card layout for its
height. A description set after the last fit, as the KOSync comparison does once the positions
are compared, leaves the heading row's layout item and the card layout with the previous text's
cached heights (Qt invalidates them only through the posted LayoutRequest chain, after the
synchronous `fitCard()`), so the card kept the "Comparing reading positions…" height and the
seven-line comparison was clipped, percentages included. Fixed in `src/ui/dialogs/modals.cpp` by
invalidating the nested rows' layouts and the card layout before measuring; the comparison's
chapter elide width now also subtracts the heading inset instead of the old card margins. Upstream
has the same code (worth reporting to Nico).

Also upstream: `.gitignore` now lists `/.claude/`, and `.claude/settings.json` (a validation hook)
is gone. New sources (`app/about/about_page.cpp`, `settings/button_controls.cpp`,
`views/appearance_preview.cpp`, `platform/api/page_buttons.h`) are picked up by the port's glob;
`app/library/about_screen.cpp` is excluded with the rest of the library.

### Launch, frame path and log off the GUI thread (2026-11-09)

A review of the port for speed, from the code and the 2026-11-01 log. Nothing in this section is measured on the device yet: each item says what to read in `kobalt.log` to judge it.

- **Launch.** `AssetBundle::available()` reads and hashes every bundled font and hyphenation file (46 MB, 26 of them the CJK font) on the main thread before the first frame. On Kobo that is once per boot; here every book is a new process. The old log has 3.9 s between `pbink: screen` and `kobalt-pb: screen` in three of four starts, 6.3 s in the fourth. The host now gives the bundle a verification record (`applications/kobalt/assets-verified.json`): a later process accepts each asset whose size and modification time match the record under the same manifest and version, which is the rule `available()` already applies within one process, and hashes anything else. A reader that returns a font's bytes still hashes its own file. What it gives up: bytes that change under an unchanged size and time (a damaged card) are no longer caught at launch. Read `kobalt-pb: assets verified in N ms`: seconds on the first start after an install, little after.
- **The compositor's copy.** `QFbScreen::doRedraw()` clears the dirty area in `mScreenImage` and then copies each window's backing store over it, the 20 ms "composing" of a page turn, for a page that is already whole in the reader window's own image. `PbInkScreen::directSource()` finds the topmost visible window, and when it covers the screen with an opaque image of the screen's format and size, `blit()` converts from that image straight into the panel's framebuffer. A dialog or other smaller window on top, or a size in the middle of a rotation, takes the compositor as before. `mScreenImage` is stale after a direct frame; the compositor redraws whatever area it is asked for, and only that area is read. A wipe's line says `direct in N ms` or `composed in N ms`. `PBINK_DIRECT=0` in the launcher switches it off to compare. To check on the device: all four rotations, the menu and the light pop-up over the page, a KOsync dialog, the keyboard, standby and wake.
- **The grey copy** now fetches the source ahead (`__builtin_prefetch`, 64 pixels) and takes 16 pixels a step. Compare `copied in N ms` with the 16 ms before.
- **The paper under the page.** `ReadingPage::paintContents()` filled the whole widget and then drew the opaque page image over most of it. It now fills only around the image. Shared code, the same pixels on every platform. Part of the 35 ms "painting"; compare `page-turn-painted` in `events.jsonl`.
- **The log.** Every line was written from the thread that logged it, and every non-debug line synced there (`fdatasync`), on the GUI thread a wait of unknown length on the flash. The gravity sensor alone logs two such lines every few seconds while the device is held. Lines are now stamped where they are logged and written, in order, by a thread of the host's (`LogWriter` in `host/main.cpp`), which syncs as before. A fatal message waits for the writer before `abort()`. Lines of inkview itself still go straight to the file, so they can now come a few milliseconds before Kobalt's lines of the same moment. A crash by signal can lose the lines of its last milliseconds.
- **Keep-awake.** The host asks at every change of any of its reasons, mostly for the state inkview already has: 3018 `keep awake` lines for 400 wipes in the old log. The plugin now compares with `GetSleepmode()` and returns, so only real changes call `iv_sleepmode()` and are logged. A book's opening shows one call holding for 13 s, so repeating the call was not what kept the device awake.
- **`IsUSBconnected()`** runs at every press and key. Its cost is unknown, so the host times it and logs `IsUSBconnected() took N ms` from 2 ms up. No such lines means it is cheap; if they appear, read the state some other way.
- **`-mtune=cortex-a7`** for Kobalt's own objects (`CMakeLists.txt`). The toolchain file is unchanged, since its flags key the dependency set and the Qt SDK. `-mcpu=cortex-a7 -mfpu=neon-vfpv4` (hardware divide, FMA) for everything is left for the next SDK rebuild, and would stop the binary running on Cortex-A8 and A9 PocketBooks.

Measured on the Era Lite the same evening, from `kobalt.log` and `events.jsonl` (copies in `backups/era-lite-after-performance-20261109` and `backups/era-lite-paint-timing-20261109` beside the checkout; the "after" samples are small):

| | Before | After |
|---|---|---|
| Start to the host's first line | 4.3 s and 6.1 s | 0.46 s and 0.42 s |
| `assets verified` | not timed | 3741 ms on the first start after the install, then 129 and 78 ms |
| Start to `book opened` | 22 s and 30 s | 15.8 s and 15.7 s |
| Composing a wipe | 22 ms | 0 (all 46 wipes direct) |
| Press to first strip, upright, not paced | 88 ms (468 turns) | 79 ms (9 turns); 63 ms (18 turns) after the strip work below |
| The same upside down | 100 ms (161) | 68 ms (5) |
| The same at 270 | 132 ms (55) | 104 ms (9) |
| `keep awake` lines | 300 for 74 wipes | 38 for 46 wipes |

No `IsUSBconnected() took` lines, so that call is cheap and stays. The prefetch and the paper fill show nothing outside the noise (copy 16 ms against 15, paint 33.8 ms against 35.8). The two `event pump took 200 ms` warnings around the standby are old: the log has 654 of them.

### The header and footer strips (2026-11-09)

Painting stayed at 34 ms for what should be one copy of the page, so `page-turn-painted` got the steps of `ReadingPage::refreshContent()` (`damage_ms`, `strips_ms`, `header_ms`, `footer_ms`, `repaint_ms`, `draw_ms`) and the text renderer's work during the strips (`text_layouts`, `text_layout_misses`, `text_glyph_misses`, `text_miss_ms`). Over 78 turns: damage 0.2 ms (the engine's prepared rectangle was used every time), repaint 16.9 ms of which 16.5 is the page's own drawing, and 15.9 ms for the strips. Both strips are rendered again at every turn, 7.5 ms each, and all 42 text layouts they ask for are found in the cache, with no glyph drawn: a third of a millisecond for each lookup that finds its answer.

Three changes to that path in `src/ui/style/private_text.cpp`, on a reading of the code, not a profile: the layout no longer builds three fonts to compare family names (`interfaceTextFamily()`), the font's part of the cache key is built once for each font (`Fonts::layoutKey()`) instead of formatting seven numbers at every call, and text drawn in black no longer copies its image to colour it black. The strips went from 15.4 to 11.7 ms (90 turns), painting from 33.9 to 30.4. The header and footer looked the same on the device. What is left needs the strips themselves changed, such as keeping each slot's image while its content stays the same; not done.

Left alone: the sleep image is composed on the GUI thread 300 ms after a lone turn (median 43 ms, worst 256 ms in the old log), which only matters if the next turn lands in that window; `sleepMode()` reads the preferences file after each turn, about a millisecond from the page cache, and caching it by file time would risk a stale answer on FAT's 2 s times. Not looked into: the 13 to 30 s from the first frame to `book opened` in the old log, which is the engine's work and the largest part of an open.

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

## Warmth schedule (2026-11-01, firmware U710.6.11.2124)

The firmware keeps its warmth curve in `system/config/frontlight/autocolor.cfg` (one `H:00 value`
line per hour; `Custom.autocolor.cfg` beside it is identical here, and `frontlight_profile=Custom`
in `global.cfg` names the profile), and a brightness curve in `autobrightness.cfg`. `libinkview.so`
has no string naming these files and no `global.cfg` key is named for the on/off state, so another
firmware process must apply the curve and keep the state. Kobalt does not read the curve.

From the device's `libinkview.so`: the on/off state is `FL_MODE_AUTO_COLOR` (2) in `hw_frontlight.mode`
of inkview's shared state (`inkinternal.h`), offset 0x4470. `hw_is_frontlight_color_auto()` returns
that bit (0 or 2, not a boolean); `hw_set_frontlight_color_auto_enabled()` sets it and sends
`MSG_SETFRONTLIGHT`, the same message `SetFrontlightColor` sends. `SetFrontlightColor` does not touch
the bit, so the host clears it on a manual change. Brightness writes do not touch it either.

Tested on the device: with the warmth at 80, ticking the option at 21:07 brought it to 65, the
curve's value for 21:00, in under 4 s (the warmth read right after the call is still the old one).
So the firmware applies the curve at once when the flag changes, with Kobalt in the foreground. A
drag then turned the flag off and held its own value (83) for the remaining minute of the test.
`hw_set_frontlight_color_auto_enabled` stayed under the 20 ms logging threshold. With the option on,
the device and Kobalt were restarted and it looked right on the device; `kobalt.log` has no line for
the state at start, so that rests on the panel.

Longer use on 0.3.3 (2026-11-02): the warmth kept following the curve across power off and on,
standby and wake, and opening and closing books. Near noon the light was visibly colder and the
firmware's frontlight sliders showed the same. That is what the user saw on the device, not read
from the log. Not established: whether the curve steps or interpolates between hours.

`SetFrontlightState` silently stores the value without applying it while automatic brightness is on
(`hw_is_frontlight_auto()`) and the keylock is off, so Kobalt's brightness slider would do nothing
in that mode. Not handled: the brightness schedule is not exposed in Kobalt.

# Kobalt on PocketBook

Kobalt runs on PocketBook e-readers as a standalone app that opens EPUBs from the device's own
library. It was ported and is tested on a PocketBook Era Lite (PB710, firmware 6.11); other
PocketBook models with the same firmware generation should work but have not been tried. This
page covers installing and using it. [Building](building.md#pocketbook) covers the build, and
[PocketBook port notes](pocketbook-notes.md) is the development log.

> [!IMPORTANT]
> Keep a backup of your books and reading data before trying it. The installer changes a few
> firmware configuration files (it keeps a copy of each next to the original) and Kobalt writes
> its progress into the library's database, exactly as the built-in reader does.

## What works

- Opening DRM-free EPUBs from the library with **Open with → Kobalt**. The built-in reader stays
  the default; Kobalt is a choice in the long-press menu.
- Kobalt's reader as on Kobo: fonts, margins, justification, dark mode, header and footer,
  bookmarks, annotations and their export, dictionaries, search, table of contents.
- The physical page-turn buttons, which also swap sides when the device is held upside down;
  holding one keeps turning pages. A short press of the power button opens Kobalt's menu.
- Automatic rotation from the gravity sensor, including starting in landscape or upside down,
  within Kobalt's rotation setting (portrait only, landscape only, or both), also for the
  loading screen and the resumed page before the book is open.
- Frontlight brightness and warmth in 1 % steps, from the light panel or the edge swipe.
- The sleep screen shows the current page or the cover with progress and battery, in the
  orientation the device is held in. The power-off screen shows the same image, and the boot
  screen shows the page as it was, when the PocketBook's Power-off logo and Boot logo settings
  (Settings → Personalize → Logos) are the book cover.
- A device that switched itself off (or was switched off from the lock screen) boots into the
  page that was being read and reopens the book in Kobalt, when the PocketBook's "Open last
  book at startup" setting is on.
- Reading progress shared with the built-in reader: the home screen shows Kobalt's progress
  and the built-in reader opens where Kobalt left off, and vice versa.
- KOsync progress sync (Settings → Sync), with Wi-Fi brought up automatically when needed. The
  sync at book open and at wake connects in the background: when the home network is out of
  reach (travel, a router down), the reader stays usable while the firmware gives up.
- Dictionary downloads from Kobalt's catalogue inside the app, and user fonts from a folder.

Not done: a proper PocketBook package format (installation is by copying files), landscape has
had little testing, and the port has only been run on one device model.

## Install

1. Download the package, [kobalt-pb.tar.gz](https://github.com/glaucon1984/kobalt/releases/latest/download/kobalt-pb.tar.gz),
   from the latest [PocketBook release](https://github.com/glaucon1984/kobalt/releases) (or
   build it, see [Building](building.md#pocketbook)), and the installer for your system:
   - Windows: [install-kobalt.ps1](https://github.com/glaucon1984/kobalt/releases/latest/download/install-kobalt.ps1),
     from the same release, into the same folder as the package.
   - macOS, Linux, or Git Bash on Windows: [install-kobalt.sh](https://github.com/glaucon1984/kobalt/blob/pocketbook/platforms/pocketbook/tools/install-kobalt.sh)
     needs the files around it, so get the whole repository: the
     [ZIP of the pocketbook branch](https://github.com/glaucon1984/kobalt/archive/refs/heads/pocketbook.zip),
     unpacked anywhere, or `git clone -b pocketbook https://github.com/glaucon1984/kobalt`.
2. Connect the PocketBook over USB and choose **PC Link**, so its storage mounts as a drive.
3. Run the installer with the device's drive and the package.

   **Windows (PowerShell, nothing else needed).** Open PowerShell in the folder with the two
   files (in Explorer, right-click the folder, *Open in Terminal*) and run, with your device's
   drive letter in place of `L:`:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\install-kobalt.ps1 L: .\kobalt-pb.tar.gz
   ```

   The `-ExecutionPolicy Bypass` part lets this one script run under Windows' default policy,
   which blocks downloaded scripts; it changes no setting. The script uses `tar.exe`, part of
   Windows since 2018. From a checkout the script is `platforms\pocketbook\tools\install-kobalt.ps1`
   and the package defaults to `build\pocketbook\kobalt-pb.tar.gz`.

   **macOS, Linux, or Git Bash on Windows.** From the unpacked ZIP or the clone:

   ```sh
   sh platforms/pocketbook/tools/install-kobalt.sh /l ~/Downloads/kobalt-pb.tar.gz
   ```

   On Windows `/l` is the `L:` drive; on macOS the mount is `/Volumes/<device name>`, on Linux
   usually `/media/<user>/<device name>`. Python 3 is needed for the home-screen step.

   Both installers do the same things and can be run again to update.
4. Eject the device. Long-press an EPUB, choose **Open with** and pick **Kobalt**.

What the installer does, in case you prefer to do it by hand:

- Puts the app in `applications/kobalt/` (binary `kobalt-pb`, `assets/`, the CA bundle
  `cacert.pem`) and the launcher script `kobalt.app` in `applications/` and in `system/bin/`.
  The firmware runs file handlers from `system/bin`, that is why the launcher is in both places.
  Kobalt's settings, bookmarks and dictionaries live in `applications/kobalt/profile/` and
  `applications/kobalt/dictionaries/`; those are kept across updates.
- Adds `kobalt.app` to the EPUB line of `system/config/extensions.cfg` (the "Open with" list),
  after the existing default. A copy of the original is kept as `extensions.cfg.before-kobalt`.
- Registers the name and icon for the Apps screen: copies two 8-bit BMP icons to
  `applications/icons/`, adds `@kobalt=Kobalt` to `system/language/en.txt` and an entry for
  `kobalt.app` to `system/config/desktop/view.json` (copy kept as `view.json.before-kobalt`).
  This part is `platforms/pocketbook/tools/pocketbook-desktop.py`.

Updating is the same command again; the profile and dictionaries are untouched.

### Uninstall

Delete `applications/kobalt/`, `applications/kobalt.app`, `system/bin/kobalt.app` and the two
`applications/icons/desktop_app_kobalt*.bmp` files, and restore `extensions.cfg` and `view.json`
from their `.before-kobalt` copies (or remove the `kobalt` entries from them). The sleep screen
image is `system/resources/Line/taskmgr_lock_background.bmp`; the original is next to it as
`taskmgr_lock_background.bmp.orig`. The power-off image is `system/logo/bookcover` (with a
`bookcover.orig` copy of what was there before). The boot logo lives in the device's flash and
is replaced by picking another in Settings → Personalize → Logos → Boot logo. The library database keeps the progress Kobalt wrote, as it
would for the built-in reader; a copy from before Kobalt's first write is
`system/explorer-3/explorer-3.db.before-kobalt`.

## Using it

- **Fonts**: drop `.ttf`, `.otf` or `.ttc` files into `applications/kobalt/fonts/` (a flat
  folder); they appear in Kobalt's font list at the next start.
- **Dictionaries**: Settings → Dictionaries lists Kobalt's catalogue and downloads inside the
  app (Wi-Fi is turned on for it). Without Wi-Fi, download the `<id>.zip` on a computer and put
  it in `applications/kobalt/dictionaries/`; it is installed at the next start.
- **KOsync**: Settings → Sync. Signing in and syncing bring Wi-Fi up through the firmware,
  which may show its network dialog the first time. Automatic sync checks the server when a
  book opens and when the device wakes, using a known network silently.
- **Wi-Fi** otherwise stays as the firmware left it; Kobalt turns it on only for the features
  above.
- **Standby**: the sleep screen is updated a moment after each page turn while the device is
  awake; the lock screen the firmware shows is therefore the page you were reading. The same
  image is copied to the firmware's power-off cover file at every standby. A capture of the page
  is stored as the boot logo (through the firmware's own tool, which takes a few seconds in the
  background) after the book opens and at each standby. When the book is opened again after a
  power-off (by the firmware at boot, or by hand), Kobalt shows that capture while the book
  loads, so the boot goes from the page to the same page. A book that was closed in Kobalt, or
  left by connecting the device to a PC, shows the usual loading screen when opened again, and
  so does any other book. A power-off
  straight from a page, without a standby first, shows the capture of the last standby or open
  at the next boot.
- **Reopening at boot**: Kobalt registers the open book with the firmware the way KOReader
  does, so the firmware's "Open last book at startup" (on by default) launches Kobalt with it
  after a power-off. Closing the book in Kobalt clears that, and the device boots to its home
  screen as usual.
- **Logs**: `applications/kobalt/kobalt.log` on the device. Attach it when reporting a problem.

## What had to change for PocketBook

Kobalt was built for Kobo devices. Almost everything in the port exists because a PocketBook
does the same things in a different way.

Getting it to run at all:

- **Building for a different kind of device.** PocketBooks run older system software than
  Kobalt expects, so the app and its graphics library (Qt) were compiled with a toolchain that
  pairs a modern compiler with the PocketBook SDK's system libraries.
- **Drawing on the screen.** Kobalt talks to a Kobo's screen directly. On a PocketBook, apps
  draw through the firmware's display layer (inkview), so a Qt platform plugin was written that
  lets Kobalt paint pages, menus and dialogs on the e-ink screen and receive touches and keys.
- **Launching from the library.** The firmware only runs reader apps installed in a specific
  place and listed in its file-type settings; the installer handles that, plus the name and icon.

Hardware that behaves differently:

- **Physical page buttons** are forwarded to the reader and swap sides upside down; a held
  button keeps turning pages, as in the built-in reader.
- **Power and menu buttons**: a short press opens Kobalt's menu.
- **The orientation sensor** reports tilt differently from a Kobo's; a bridge lets Kobalt see a
  real sensor, so auto-rotation works, including starting the app upside down or in landscape.
- **Full screen refreshes.** The PocketBook's screen does not clear itself the way a Kobo's does,
  so the reader asks for a full refresh after a rotation and after sync dialogs close, to get
  rid of ghosting.
- **Frontlight**: the PocketBook's finer warmth control is exposed in 1 % steps, and a bug that
  turned the light off on exit was fixed.

Sleep and standby:

- **The sleep screen was re-engineered.** A Kobo lets the reader draw its own sleep screen. The
  PocketBook firmware shows a fixed image it loads when the app starts, so Kobalt writes its
  sleep image into that file after every page turn and asks the firmware to reload it, timed so
  a lock pressed right after a page turn does not catch the device half asleep. The same image
  goes to the firmware's power-off cover file, and a capture of the page becomes the boot logo
  through the firmware's own tool, so neither screen is left showing another app's picture of
  another book.
- **Reopening the book at boot.** The firmware remembers which book a task has open only when
  the app tells it (a marker file and the task's subtask info, as KOReader does); Kobalt does so
  now, so a device that switched itself off boots into the page and reopens the book.
- **The device falling asleep mid-task.** A PocketBook goes into light sleep a few seconds after
  the last touch, even while the app is working; books stalled on the loading screen because of
  it. Kobalt keeps the device awake while opening a book, downloading or syncing.
- **Saving before standby and USB.** Progress is saved and flushed every time the device sleeps
  or is plugged into a computer, because the USB file transfer was seen to lose unsaved writes.

Reading and the library:

- **Page-turn animation re-engineered.** Kobo screens have a hardware sliding effect that the
  PocketBook lacks, so a strip-by-strip wipe was written, paced so quick turns do not tear.
- **Progress shared with the built-in reader**, through the library's own database and the
  built-in reader's position format.
- **Fonts** from a dedicated folder, since the firmware's font folder is laid out differently.

Network:

- **Wi-Fi on demand**, since the firmware keeps it off until an app asks.
- **Secure connections**: the firmware ships no certificates Kobalt can use, so the app carries
  its own, which makes dictionary downloads and KOsync possible.

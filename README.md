<p align="center">
  <img src="assets/kobalt.svg" alt="Kobalt" width="320" height="208">
</p>

**Kobalt is an alternative EPUB reader with a CREngine renderer and a Qt reading interface, with a focus on speed and an excellent user experience.**

> [!NOTE]
> This is a fork of [nicoverbruggen/kobalt](https://github.com/nicoverbruggen/kobalt) that adds a **PocketBook** version, on the `pocketbook` branch. Everything else is Nico Verbruggen's work; the Kobo, Android and macOS versions are his and are documented below unchanged. See [PocketBook](#pocketbook) and [Kobalt on PocketBook](doc/pocketbook.md).

The reader app is its own thing, and as such is planned to be ported to various platforms. On Android, it runs as a standalone app, and on Kobo it runs somewhat integrated into Kobo's own OS.

### How does the Kobo version work?

The Kobo version opens DRM-free EPUBs, including DRM-free books purchased from Kobo. Kobo's OS still handles dictionaries, reading progress and reading sessions. You can choose Kobalt as your default reader or open a book with it from the book's long-press menu.

Protected books, previews, PDFs and other unsupported formats show “Opening in built-in reader” and continue in Kobo's regular reader app.

> [!NOTE]
> This project intentionally contains no service for loading decrypted resources from protected books. Books with any kind of DRM are automatically opened by the built-in reader.

### What about the other versions?

The Android version is now available for testing, as of beta 4. You can download the APK file and install it manually. For the other versions, the source code is included in this repository. The Mac version is not ready for general release yet, it's for local testing.

#### Android

The Android version has a Library feature that can scan a directory for books, but it also opens unprotected EPUBs through the document picker or Open with. It keeps reading progress and bookmarks locally. See [Android setup, building, signing and limitations](doc/building.md#android) for more information.

#### macOS

The macOS development app uses the same reader in a resizable window for testing books. For more information, take a look at [the documentation](doc/building.md#mac).

#### PocketBook

The PocketBook version is this fork's addition: Kobalt's reader as a standalone app on PocketBook e-readers. It opens DRM-free EPUBs from the device's library through **Open with**, shares reading progress with the built-in reader, and brings Kobalt's dictionaries, KOsync, fonts, frontlight control and sleep screen to the device. It is developed and tested on a PocketBook Era Lite. [Kobalt on PocketBook](doc/pocketbook.md) has the installation steps; [Building](doc/building.md#pocketbook) the build; the [port notes](doc/pocketbook-notes.md) are the development log, written for the next person (or agent) who works on the port.

Kobalt was built for Kobo devices, and almost everything in the port exists because a PocketBook does the same things differently. What had to change, in plain words:

- **Building for a different kind of device.** PocketBooks run older system software than Kobalt expects, so the app and Qt were compiled with a toolchain that pairs a modern compiler with the PocketBook SDK's system libraries.
- **Drawing on the screen.** Kobalt talks to a Kobo's screen directly. On a PocketBook, apps draw through the firmware's display layer, so a Qt platform plugin was written that paints Kobalt's pages, menus and dialogs on the e-ink screen and delivers touches and keys.
- **Launching from the library.** The firmware only runs reader apps installed in a specific place and listed in its file-type settings; the installer handles that, plus the name and icon on the Apps screen.
- **Physical page buttons** did nothing at first. They are forwarded to the reader, swap sides when the device is upside down and keep turning pages while held. A short press of the power button opens Kobalt's menu.
- **The orientation sensor** reports tilt differently from a Kobo's. A bridge lets Kobalt see a real sensor, so auto-rotation works, including starting the app upside down or in landscape.
- **Full screen refreshes.** The PocketBook's screen does not clear itself the way a Kobo's does, so the reader asks for a full refresh after a rotation and after sync dialogs close, to get rid of ghosting.
- **Frontlight** warmth in 1 % steps instead of Kobo's ten levels, and a bug that turned the light off on exit was fixed.
- **The sleep screen was re-engineered.** A Kobo lets the reader draw its own sleep screen. The PocketBook firmware shows a fixed image loaded when the app starts, so Kobalt writes its sleep image (page or cover, progress, battery, in the device's orientation) into that file after every page turn and asks the firmware to reload it, timed so a lock right after a page turn does not catch the device half asleep. The power-off screen gets the same image, and the boot screen a capture of the page.
- **Reopening the book at boot.** A PocketBook switches itself off after a long standby. Kobalt registers the open book with the firmware (as KOReader does), so the device boots into the page that was being read and reopens the book in Kobalt.
- **The device falling asleep mid-task.** A PocketBook goes into light sleep a few seconds after the last touch, even while an app is working; books stalled on the loading screen because of it. Kobalt keeps the device awake while opening a book, downloading or syncing.
- **Saving before standby and USB.** Progress is saved and flushed every time the device sleeps or is plugged into a computer, because the USB file transfer was seen to lose unsaved writes.
- **Page-turn animation re-engineered.** Kobo screens have a hardware sliding effect the PocketBook lacks, so a strip-by-strip wipe was written, paced so quick turns do not tear.
- **Progress shared with the built-in reader**, through the library's own database and the built-in reader's position format, so the home screen is right and either reader opens where the other left off.
- **Wi-Fi on demand**, since the firmware keeps it off until an app asks: dictionary downloads and KOsync turn it on, and sync reconnects silently to a known network when a book opens or the device wakes.
- **Secure connections.** The firmware ships no certificates Kobalt can use, so the app carries its own.

## Compatibility and installation

### Install on Kobo

> [!IMPORTANT]
> Keep a backup of your books and reading data before testing, no matter whether you use the website or not. I have done my best to make Kobalt as stable and safe as possible, but you should always have a backup, just to be safe.

#### Automatic installation via Chrome

The easiest way is via [the website](https://kobalt.nicoverbruggen.be). Click on **Install or remove on Kobo** and you should follow the steps on the website. The website will check if your device is compatible.

#### Manual installation

There are two installation packages: one for the firmware 4.x line, and one for the latest beta, which is currently at version 6.x. Choose the right one for your Kobo, so check what software version you have installed first!

| Package | Firmware
|---|---|
| `Kobalt-qt5.zip` | 4.38.21908 and later 4.x (tested up to 4.46)
| `Kobalt-qt6.zip` | 5.18 and later 5.x, and 6.x (tested up to 6.0.2)

_(*) Due to how software updates work on Kobo OS 6, you may need a future version of [NickelModManager](https://github.com/nicoverbruggen/NickelModManager) to avoid needing to reinstall mods._

1. Connect your Kobo over USB and extract the ZIP package on your computer. Show hidden files so you can see `.adds` and `.kobo`.
2. Copy the complete `.adds` folder to the root of the Kobo drive. Merge it with the existing folder and replace matching package files. Keep other apps, settings and reading data.
3. Copy the complete `.kobo` folder from the extracted package to the same drive root. Merge it with the existing folder. Firmware 4 uses `.kobo/KoboRoot.tgz`; firmware 5 and 6 use `.kobo/Kobo.tgz`.
4. Eject the Kobo safely, disconnect USB and let it install the update.

Copy every file supplied in the ZIP, keeping its folder path. Copy `.adds` before `.kobo` so the assets are present when the update starts. Do not copy only the `.tgz` file or unpack it yourself. Kobalt creates its support files and installation marker on first startup.

### Removing Kobalt from Kobo devices

#### Automatic removal via Chrome

Like the installation, the easiest way is via [the website](https://kobalt.nicoverbruggen.be). Click on **Install or remove on Kobo** and you should follow the steps on the website. Removal is the easiest this way.

#### Manual removal

You can also connect your device via USB and remove the uninstallation file (`.adds/kobalt/uninstall`) which will cause Kobalt to remove itself but retain your configuration files, annotations and more. If you would like to remove _everything_, remove `.adds/kobalt` in its entirety. After removing either of these, make sure to safely eject your device.

### Install on PocketBook

Download `kobalt-pb.tar.gz` from the [PocketBook releases](https://github.com/glaucon1984/kobalt/releases), connect the device over USB (PC Link) and run `platforms/pocketbook/tools/install-kobalt.sh <mount> <package>` from a checkout of this repository. The steps, what the installer changes and how to undo it are in [Kobalt on PocketBook](doc/pocketbook.md#install).

## Issues and contributions

This repository contains Kobalt's public source. Report bugs via the Discussions tab here on GitHub. Issues are reserved for contributors for managing the project.

If you have a sample book that exhibits an issue, please attach it if you are permitted to do so, this can help find issues more quickly.

## Acknowledgements

For a complete list, please check out [the acknowledgements](doc/acknowledgements.md) document.

### KOReader

[KOReader](https://koreader.rocks/) is an amazing piece of software, and some of its source is used (the renderer, for example) to make this project possible. A big thanks to the contributors of KOReader. If you feel like Kobalt is too limited in its features, you may want to give it a shot. 

## Disclaimers

1. This project is not affiliated with Rakuten Kobo Inc. or the tolino alliance. Kobo and tolino are trademarks of their respective owners.
2. The creator of Kobalt does not encourage or endorse the removal of DRM, and Kobalt does not allow for it. Support your favorite author by purchasing their books.
3. This project was made with the help of agentic engineering with models from OpenAI and Anthropic. I would not have been able to make this project otherwise. Instructions intended for agents are included in [AGENTS.md](AGENTS.md).

## License

Like KOReader, this project is available under an open source license.

Kobalt's own source code uses the MIT license in [LICENSE](LICENSE). CREngine patches use GNU GPL v2 or later, and Qt patches use Qt's licenses. Third-party code and assets retain their own licenses and notices in `assets/notices/` and `platforms/kobo/toolchains/nickelhook-qt6/`.

The combined package includes GPL and AGPL code and is distributed under GNU AGPL v3, without warranty. Binary releases include the corresponding source and dependency sources.

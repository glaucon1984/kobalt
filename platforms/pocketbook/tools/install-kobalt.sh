#!/bin/sh
# Install Kobalt on a PocketBook mounted over USB (Git Bash on Windows, or any POSIX shell):
# applications/kobalt.app (launcher) + applications/kobalt/ (binary, assets, profile), and an
# "Open with" entry for EPUBs in system/config/extensions.cfg (the existing default reader stays
# the default). Then tools/pocketbook-desktop.py registers the name and icon on the home screen.
# Usage: platforms/pocketbook/tools/install-kobalt.sh [mount, default /l] [package, default
#        build/pocketbook/kobalt-pb.tar.gz or a downloaded release]
set -eu
here=$(CDPATH= cd "$(dirname "$0")" && pwd)
root=$(CDPATH= cd "$here/../../.." && pwd)
mnt=${1:-/l}
pkg=${2:-$root/build/pocketbook/kobalt-pb.tar.gz}
[ -d "$mnt/applications" ] || { echo "no PocketBook at $mnt"; exit 1; }
[ -f "$pkg" ] || { echo "$pkg missing: download a release or run tools/remote-build-kobalt.sh"; exit 1; }
tmp=$(mktemp -d)
tar xzf "$pkg" -C "$tmp"
mkdir -p "$mnt/applications/kobalt"
cp "$tmp/kobalt.app" "$mnt/applications/kobalt.app"
# The firmware runs file handlers from system/bin (KOReader and better-stats register there too).
mkdir -p "$mnt/system/bin"
cp "$tmp/kobalt.app" "$mnt/system/bin/kobalt.app"
mkdir -p "$mnt/applications/kobalt/fonts"
cp "$tmp/kobalt/kobalt-pb" "$mnt/applications/kobalt/kobalt-pb"
[ -f "$tmp/kobalt/cacert.pem" ] && cp "$tmp/kobalt/cacert.pem" "$mnt/applications/kobalt/cacert.pem"
rm -rf "$mnt/applications/kobalt/assets"
cp -r "$tmp/kobalt/assets" "$mnt/applications/kobalt/assets"
rm -rf "$tmp"
cfg="$mnt/system/config/extensions.cfg"
if [ -f "$cfg" ] && ! grep -q "kobalt.app" "$cfg"; then
    cp "$cfg" "$cfg.before-kobalt"
    # epub:@EPUB_file:1:koreader.app,eink-reader.app:ICON_EPUB -> add kobalt.app after the first handler
    sed -i -E '/^epub:/ s/^(epub:[^:]*:[^:]*:[^,:]+)/\1,kobalt.app/' "$cfg"
    echo "extensions.cfg epub line now: $(grep '^epub:' "$cfg")"
fi
python3 "$here/pocketbook-desktop.py" "$mnt" ${KOBALT_RESET_HANDLERS:+--reset-handlers}
ls -la "$mnt/applications/kobalt.app" "$mnt/applications/kobalt/kobalt-pb"
du -sh "$mnt/applications/kobalt/assets"
echo "installed; eject, long-press an EPUB > Open with > kobalt"

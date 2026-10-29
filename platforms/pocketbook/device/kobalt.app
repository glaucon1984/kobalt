#!/bin/sh
# PocketBook launcher for Kobalt (spike). Lives in /mnt/ext1/applications/; the firmware passes
# the book path as the first argument when a file is opened with it.
DIR=/mnt/ext1/applications/kobalt
echo "launcher: $(date) pid $$ cwd $(pwd) argv: $0 $*" >> "$DIR/kobalt.log"
export GLIBC_TUNABLES="${GLIBC_TUNABLES}${GLIBC_TUNABLES:+:}glibc.rtld.execstack=2"
export LC_ALL="en_US.UTF-8"
export QT_QPA_PLATFORM="pbink:debug"
# TLS roots for Kobalt's network features (dictionary downloads, sync); the firmware ships none Qt finds.
export SSL_CERT_FILE="$DIR/cacert.pem"
export SSL_CERT_DIR="$DIR"   # Qt scans this directory for *.pem root certificates
export QT_LOGGING_RULES="qt.qpa.pbink.debug=true"
cd "$DIR" || exit 1
# Started from Apps without a book: open the path written in open.cfg, if any (test convenience).
if [ $# -eq 0 ] && [ -s "$DIR/open.cfg" ]; then
    set -- "$(head -n 1 "$DIR/open.cfg" | tr -d '\r')"
fi
exec "$DIR/kobalt-pb" "$@" >> "$DIR/kobalt.log" 2>&1

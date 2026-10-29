#!/bin/sh
# PocketBook launcher for the pbqt spike. Lives in /mnt/ext1/applications/ so that the firmware
# lists it under Apps; the binary and its files are in applications/pbqt/.
# Same loader workaround as KOReader's launcher for firmware 6.11 (executable-stack check).
export GLIBC_TUNABLES="${GLIBC_TUNABLES}${GLIBC_TUNABLES:+:}glibc.rtld.execstack=2"
export LC_ALL="en_US.UTF-8"
DIR=/mnt/ext1/applications/pbqt
# Event and refresh logging from the platform plugin (drop both lines to quieten the log).
export QT_QPA_PLATFORM="pbink:debug"
export QT_LOGGING_RULES="qt.qpa.pbink.debug=true"
cd "$DIR" || exit 1
echo "launcher: $(date) args: $*" >> "$DIR/pbqt.log"
exec "$DIR/pbqt-hello" "$@" >> "$DIR/pbqt.log" 2>&1

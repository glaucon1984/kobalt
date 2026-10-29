#!/bin/bash
# Configure, build and install a static Qt 5.15 qtbase for the PocketBook B288 toolchain.
# Runs inside the pbqt-build container with ~/build/pbqt mounted at /work.
set -euo pipefail
SDK=/work/sdk/SDK-B288
SRC=/work/qtbase-everywhere-src-5.15.19
PREFIX=/work/qt-pb
JOBS=${JOBS:-8}

mkdir -p /work/qt-build
cd /work/qt-build
if [ ! -f config.status ]; then
  "$SRC/configure" -opensource -confirm-license -release -static \
    -prefix "$PREFIX" \
    -device linux-generic-g++ \
    -device-option CROSS_COMPILE="$SDK/usr/bin/arm-obreey-linux-gnueabi-" \
    -c++std c++14 \
    -no-pkg-config -no-pch -no-use-gold-linker \
    -nomake examples -nomake tests \
    -no-opengl -no-xcb -no-eglfs -no-kms -no-gbm -no-vulkan -no-libinput -no-tslib -no-mtdev \
    -linuxfb -evdev \
    -qt-freetype -no-fontconfig -qt-libpng -qt-libjpeg -qt-zlib -qt-pcre -qt-harfbuzz \
    -no-dbus -no-glib -no-icu -no-cups -no-openssl \
    -sql-sqlite -qt-sqlite -no-sql-mysql -no-sql-psql -no-sql-odbc \
    -no-feature-vnc || {
      echo "=== configure failed; .qmake.cache:"; cat .qmake.cache 2>/dev/null
      echo "=== config.log tail:"; tail -60 config.log 2>/dev/null
      exit 3
    }
fi
make -j"$JOBS"
make install
echo "QT BUILD OK"

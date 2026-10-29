#!/bin/bash
# Static Qt 5.15 qtbase + qtsensors for PocketBook with GCC 12 and Kobalt's own FreeType, HarfBuzz,
# libpng and libjpeg (from /work/kobalt-deps/install), so that Kobalt and Qt share one copy of each.
# Runs in the pbqt-build container; installs to /work/qt-pb12. The archives are named by path:
# with -l names the sysroot's old shared libfreetype would win over Kobalt's static one.
set -euo pipefail
SRC=/work/qtbase-everywhere-src-5.15.19
PREFIX=/work/qt-pb12
DEPS=/work/kobalt-deps/install
JOBS=${JOBS:-8}
bash "$(dirname "$0")/build-glibc-compat.sh"
mkdir -p "$SRC/mkspecs/devices/linux-pocketbook-g++"
cp "$(dirname "$0")"/mkspec/linux-pocketbook-g++/* "$SRC/mkspecs/devices/linux-pocketbook-g++/"
mkdir -p /work/qt-build-gcc12 && cd /work/qt-build-gcc12
if [ ! -f config.status ]; then
  "$SRC/configure" -opensource -confirm-license -release -static \
    -prefix "$PREFIX" \
    -device linux-pocketbook-g++ -device-option CROSS_COMPILE=arm-linux-gnueabi- \
    -c++std c++17 \
    -no-pkg-config -no-pch -no-use-gold-linker \
    -nomake examples -nomake tests \
    -no-opengl -no-xcb -no-eglfs -no-kms -no-gbm -no-vulkan -no-libinput -no-tslib -no-mtdev \
    -linuxfb -evdev \
    -system-freetype -system-harfbuzz -system-libpng -system-libjpeg -system-zlib -no-fontconfig \
    -qt-pcre \
    -no-dbus -no-glib -no-icu -no-cups -openssl-linked "OPENSSL_INCDIR=/work/openssl-pb/include" "OPENSSL_LIBS=/work/openssl-pb/lib/libssl.a /work/openssl-pb/lib/libcrypto.a -ldl -lpthread" \
    -sql-sqlite -qt-sqlite -no-sql-mysql -no-sql-psql -no-sql-odbc \
    -no-feature-vnc \
    "FREETYPE_INCDIR=$DEPS/include/freetype2" "FREETYPE_LIBS=$DEPS/lib/libfreetype.a $DEPS/lib/libpng16.a -lz" \
    "HARFBUZZ_INCDIR=$DEPS/include" "HARFBUZZ_LIBS=$DEPS/lib/libharfbuzz.a $DEPS/lib/libfreetype.a $DEPS/lib/libpng16.a -lz" \
    "LIBPNG_INCDIR=$DEPS/include" "LIBPNG_LIBS=$DEPS/lib/libpng16.a -lz" \
    "LIBJPEG_INCDIR=$DEPS/include" "LIBJPEG_LIBS=$DEPS/lib/libjpeg.a" || {
      echo "=== configure failed"; tail -60 config.log; exit 3; }
  grep -E "FreeType|HarfBuzz|PNG|JPEG|zlib|Accessibility|C\+\+ standard|Compiler" config.summary | head -20
fi
make -j"$JOBS"
make install
echo "QTBASE OK"
# Qt Sensors on top
cd /work
[ -d qtsensors-everywhere-src-5.15.19 ] || tar xJf qtsensors-5.15.19.tar.xz
rm -rf qtsensors-build-gcc12 && mkdir qtsensors-build-gcc12 && cd qtsensors-build-gcc12
"$PREFIX/bin/qmake" ../qtsensors-everywhere-src-5.15.19
make -j"$JOBS"
make install
ls "$PREFIX/lib/libQt5Sensors.a"
echo "QT GCC12 BUILD OK"

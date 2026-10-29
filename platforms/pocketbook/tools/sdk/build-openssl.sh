#!/bin/bash
# Static OpenSSL for the PocketBook sysroot with the GCC 12 toolchain (same version Kobalt pins
# for Android). Installs to /work/openssl-pb. Runs in the pbqt-build container.
set -euo pipefail
VER=3.5.9
SYSROOT=/work/sdk/SDK-B288/usr/arm-obreey-linux-gnueabi/sysroot
G=/usr/lib/gcc-cross/arm-linux-gnueabi/12
cd /work
[ -d openssl-openssl-$VER ] || tar xzf openssl-$VER.tar.gz
rm -rf openssl-build && cp -a openssl-openssl-$VER openssl-build && cd openssl-build
export CC=arm-linux-gnueabi-gcc AR=arm-linux-gnueabi-ar RANLIB=arm-linux-gnueabi-ranlib
export CFLAGS="--sysroot=$SYSROOT -B$SYSROOT/usr/lib/ -nostdinc -isystem $G/include -isystem $SYSROOT/usr/include -march=armv7-a -mfpu=neon -mfloat-abi=softfp -fsigned-char -fno-pie -O2"
export LDFLAGS="--sysroot=$SYSROOT -B$SYSROOT/usr/lib/ -L$G -L$SYSROOT/lib -L$SYSROOT/usr/lib -no-pie"
./Configure linux-armv4 no-shared no-tests no-apps no-docs no-legacy no-module no-dso threads \
  --prefix=/work/openssl-pb --libdir=lib --openssldir=/etc/ssl
make -j"${JOBS:-8}" build_libs
make install_dev
ls /work/openssl-pb/lib/libssl.a /work/openssl-pb/lib/libcrypto.a /work/openssl-pb/include/openssl/ssl.h
echo "OPENSSL OK"

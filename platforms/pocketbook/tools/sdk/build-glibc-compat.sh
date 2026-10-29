#!/bin/bash
# Compile tools/sdk/glibc-compat.c into /work/gcc12-compat/glibc_compat.o with GCC 12 for the sysroot.
set -euo pipefail
SYSROOT=/work/sdk/SDK-B288/usr/arm-obreey-linux-gnueabi/sysroot
mkdir -p /work/gcc12-compat
arm-linux-gnueabi-gcc --sysroot=$SYSROOT -nostdinc -isystem /usr/lib/gcc-cross/arm-linux-gnueabi/12/include -isystem $SYSROOT/usr/include \
  -march=armv7-a -mfpu=neon -mfloat-abi=softfp -fsigned-char -fno-pie -O2 -c "$(dirname "$0")/glibc-compat.c" -o /work/gcc12-compat/glibc_compat.o
arm-linux-gnueabi-nm /work/gcc12-compat/glibc_compat.o

#!/bin/bash
# libstdc++ 12's c++config.h was generated against glibc 2.36; the PocketBook sysroot has 2.23.
# Produce an overlay copy with the newer-glibc features switched off, to be put in front of the
# real header directory with -isystem. (A toolchain built against the sysroot would not need this.)
set -euo pipefail
src=/usr/arm-linux-gnueabi/include/c++/12/arm-linux-gnueabi/bits/c++config.h
out=${1:-/work/gcc12-overlay}
mkdir -p "$out/bits"
sed -E 's/^#define (_GLIBCXX_USE_PTHREAD_COND_CLOCKWAIT|_GLIBCXX_USE_PTHREAD_MUTEX_CLOCKLOCK|_GLIBCXX_USE_PTHREAD_RWLOCK_CLOCKLOCK|_GLIBCXX_HAVE_GETENTROPY|_GLIBCXX_HAVE_ARC4RANDOM) 1$/\/* #undef \1 (glibc 2.23 sysroot) *\//' "$src" > "$out/bits/c++config.h"
grep -c "glibc 2.23 sysroot" "$out/bits/c++config.h"

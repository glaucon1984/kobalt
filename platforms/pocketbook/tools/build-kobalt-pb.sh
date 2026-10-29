#!/bin/bash
# Configure, build and package Kobalt for PocketBook inside the pbqt-build container (SDK image,
# tools/sdk/Dockerfile), with the repository synced to /work/kobalt by tools/remote-build-kobalt.sh.
# Usage: build-kobalt-pb.sh [configure|build|package|all]
# Env: BUILD_DIR (default /work/kobalt-pb-build), PB_QT (default /work/qt-pb12),
#      KOBALT_COMMIT / KOBALT_COMMIT_MODIFIED (passed by the launcher for the About screen)
set -uo pipefail
what=${1:-all}
B=${BUILD_DIR:-/work/kobalt-pb-build}
QT=${PB_QT:-/work/qt-pb12}
SRC=${KOBALT_SRC:-/work/kobalt}
PB=$SRC/platforms/pocketbook
mkdir -p "$B" && cd "$B"
if [ ! -f CMakeCache.txt ] || [ "$what" = configure ]; then
  cmake -G Ninja -S "$PB" \
    -DCMAKE_TOOLCHAIN_FILE="$PB/tools/pocketbook-gcc12.toolchain.cmake" "-DPB_QT=$QT" \
    -DCMAKE_BUILD_TYPE=Release > configure.log 2>&1 || { echo "CONFIGURE FAILED"; tail -40 configure.log; exit 2; }
  echo "configure ok"
fi
# The commit stamp follows every build (the cache would keep the last one otherwise).
cmake "-DKOBALT_COMMIT=${KOBALT_COMMIT:-}" "-DKOBALT_COMMIT_MODIFIED=${KOBALT_COMMIT_MODIFIED:-OFF}" . > /dev/null || exit 2
[ "$what" = configure ] && exit 0
cmake --build . --target kobalt-pb -- -k 0 > build.log 2>&1; rc=$?
echo "build exit $rc"
if [ $rc -ne 0 ]; then
  echo "=== errors"; grep -E "error:|undefined reference|multiple definition|FAILED:" build.log | sed -E "s#$SRC/##g" | sort | uniq -c | sort -rn | head -40 | cut -c1-240
  exit 1
fi
grep -E "warning:" build.log | grep -E "platforms/pocketbook" | sed -E "s#$SRC/##g" | sort -u | head -20
cmake --build . --target package > package.log 2>&1 && ls -la kobalt-pb.tar.gz package/kobalt/kobalt-pb && echo "PACKAGE OK"

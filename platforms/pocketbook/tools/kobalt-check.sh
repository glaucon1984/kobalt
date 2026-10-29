#!/bin/bash
# Configure and build the Kobalt compile check inside the pbqt-build container.
# Usage: kobalt-check.sh [deps|src|all]   (logs in $BUILD_DIR, default /work/kobalt-check-build/)
# Env: BUILD_DIR, TOOLCHAIN (default tools/pocketbook-sdk.toolchain.cmake), CMAKE_EXTRA (more -D flags)
set -uo pipefail
what=${1:-all}
B=${BUILD_DIR:-/work/kobalt-check-build}
PB=${KOBALT_SRC:-/work/kobalt}/platforms/pocketbook
TC=${TOOLCHAIN:-$PB/tools/pocketbook-sdk.toolchain.cmake}
mkdir -p "$B" && cd "$B"
if [ ! -f CMakeCache.txt ]; then
  cmake -G Ninja -S "$PB/tools/kobalt-check" \
    "-DCMAKE_TOOLCHAIN_FILE=$TC" \
    -DCMAKE_BUILD_TYPE=Release -DKOBALT_DEPENDENCY_JOBS=8 ${CMAKE_EXTRA:-} > configure.log 2>&1 || {
      echo "CONFIGURE FAILED"; tail -40 configure.log; exit 2; }
  echo "configure ok"
fi
if [ "$what" = deps ] || [ "$what" = all ]; then
  cmake --build . --target kobalt-dependencies -- -k 0 > deps.log 2>&1; rc=$?
  echo "deps build exit $rc"
  grep -E "FAILED:|error:" deps.log | head -40
fi
if [ "$what" = src ] || [ "$what" = all ]; then
  cmake --build . --target kobalt-check -- -k 0 > src.log 2>&1; rc=$?
  echo "src build exit $rc"
  echo "objects built: $(find CMakeFiles/kobalt-check.dir -name '*.o' | wc -l)"
  echo "files with errors: $(grep -oE '^[^:]+\.(cpp|h):[0-9]+:[0-9]+: error' src.log | cut -d: -f1 | sort -u | wc -l)"
  echo "error lines: $(grep -c ': error:' src.log)"
fi

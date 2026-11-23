#!/bin/bash
# Rebuild Kobalt's third-party libraries (FreeType, HarfBuzz, libpng, libwebp, Brotli, Lua and
# friends) for the repository synced to /work/kobalt with the GCC 12 toolchain, then refresh
# /work/kobalt-deps/install, the prefix the PocketBook build links against. Needed whenever
# upstream moves dependencies/manifest.json, dependencies/patches or the recipes. Runs inside the
# pbqt-build container; tools/remote-rebuild-deps.sh runs it from the PC.
# Upstream's dependency cache (dependencies/cache.cmake) builds the set at configure time into
# /work/dependency-cache/<key>/, keyed by the toolchain and the recipes, so an unchanged set is
# found at once and a changed one is built in full (about half an hour).
# The reader engine is not part of the set: the application build compiles it from the repository.
# Env: BUILD_DIR (default /work/kobalt-check-gcc12), KOBALT_SRC (default /work/kobalt)
set -uo pipefail
B=${BUILD_DIR:-/work/kobalt-check-gcc12}
D=/work/kobalt-deps
PB=${KOBALT_SRC:-/work/kobalt}/platforms/pocketbook
mkdir -p "$B" && cd "$B"
# A cache left by another source path (an older layout) cannot be reused.
if [ -f CMakeCache.txt ] && ! grep -q "^CMAKE_HOME_DIRECTORY:INTERNAL=$PB/tools/kobalt-check\$" CMakeCache.txt; then
  echo "stale build directory (other source path): starting afresh"
  rm -rf "$B"/*
fi
cmake -G Ninja -S "$PB/tools/kobalt-check" \
  -DCMAKE_TOOLCHAIN_FILE="$PB/tools/pocketbook-gcc12.toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DKOBALT_DEPENDENCY_JOBS=8 -DKOBALT_POCKETBOOK=ON \
  -DKOBALT_DEPENDENCY_CACHE=/work/dependency-cache > configure.log 2>&1 || {
    echo "CONFIGURE FAILED"; tail -40 configure.log; exit 2; }
echo "configure ok"
entry=$(sed -n 's/^-- Dependency set [0-9a-f]*: //p' configure.log | tail -1)
[ -n "$entry" ] && [ -f "$entry/complete" ] || { echo "no published dependency set in configure.log"; tail -20 configure.log; exit 1; }
echo "dependency set: $entry"
install=$entry/install
[ -d "$install/include" ] || { echo "no install prefix at $install"; exit 1; }
# The install prefix (FreeType, HarfBuzz, libpng, ... also linked into Qt) follows the tree. Its
# headers changing means a library moved and Qt should be rebuilt too (tools/build-qt-gcc12.sh);
# archives alone differ at every rebuild (ar timestamps) and need nothing.
if diff -rq "$install/include" "$D/install/include" > install.diff 2>&1; then
  echo "install prefix headers unchanged (archives refreshed; Qt needs no rebuild)"
else
  echo "install prefix headers differ (rebuild Qt with tools/build-qt-gcc12.sh):"; head -20 install.diff
fi
rm -rf "$D/install.new" && cp -a "$install" "$D/install.new" || { echo "copy failed"; exit 1; }
rm -rf "$D/install.old"
[ -d "$D/install" ] && mv "$D/install" "$D/install.old"
mv "$D/install.new" "$D/install"
echo "DEPS OK"

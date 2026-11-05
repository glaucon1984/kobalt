#!/bin/bash
# Rebuild Kobalt's third-party libraries (CREngine and friends) for the repository synced to
# /work/kobalt with the GCC 12 toolchain, then refresh /work/kobalt-deps (crengine-src/,
# crengine-build/, install/), which the PocketBook build links against. Needed whenever upstream
# moves dependencies/manifest.json or dependencies/patches (a CREngine upgrade, a new patch). Runs
# inside the pbqt-build container; tools/remote-rebuild-deps.sh runs it from the PC.
# Upstream's dependency cache (dependencies/cache.cmake) builds the set at configure time into
# /work/dependency-cache/<key>/, keyed by the toolchain and the recipes, so an unchanged set is
# found at once and a changed one is built in full (about half an hour).
# Env: BUILD_DIR (default /work/kobalt-check-gcc12)
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
src=$entry/sources/crengine
build=$entry/crengine
install=$entry/install
[ -f "$src/crengine/include/lvtinydom.h" ] || { echo "no CREngine source at $src"; exit 1; }
[ -f "$build/libcrengine.a" ] || { echo "no libcrengine.a at $build"; exit 1; }
[ -d "$install/include" ] || { echo "no install prefix at $install"; exit 1; }
rm -rf "$D/crengine-src.new" "$D/crengine-build.new"
cp -a "$src" "$D/crengine-src.new" && cp -a "$build" "$D/crengine-build.new" || { echo "copy failed"; exit 1; }
rm -rf "$D/crengine-src.old" "$D/crengine-build.old"
[ -d "$D/crengine-src" ] && mv "$D/crengine-src" "$D/crengine-src.old"
[ -d "$D/crengine-build" ] && mv "$D/crengine-build" "$D/crengine-build.old"
mv "$D/crengine-src.new" "$D/crengine-src" && mv "$D/crengine-build.new" "$D/crengine-build"
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

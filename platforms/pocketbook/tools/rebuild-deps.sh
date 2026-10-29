#!/bin/bash
# Rebuild Kobalt's third-party libraries (CREngine and friends) for the repository synced to
# /work/kobalt with the GCC 12 toolchain, then refresh /work/kobalt-deps (crengine-src/,
# crengine-build/, install/), which the PocketBook build links against. Needed whenever upstream
# moves dependencies/manifest.json or dependencies/patches (a CREngine upgrade, a new patch). Runs
# inside the pbqt-build container; tools/remote-rebuild-deps.sh runs it from the PC. Unchanged
# libraries are not rebuilt (same source hash, same recipe), so a CREngine-only change takes a few
# minutes.
# Env: BUILD_DIR (default /work/kobalt-check-gcc12)
set -uo pipefail
B=${BUILD_DIR:-/work/kobalt-check-gcc12}
D=/work/kobalt-deps
PB=${KOBALT_SRC:-/work/kobalt}/platforms/pocketbook
mkdir -p "$B" && cd "$B"
if [ ! -f CMakeCache.txt ]; then
  cmake -G Ninja -S "$PB/tools/kobalt-check" \
    -DCMAKE_TOOLCHAIN_FILE="$PB/tools/pocketbook-gcc12.toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DKOBALT_DEPENDENCY_JOBS=8 -DKOBALT_POCKETBOOK=ON > configure.log 2>&1 || {
      echo "CONFIGURE FAILED"; tail -40 configure.log; exit 2; }
  echo "configure ok"
fi
cmake --build . --target kobalt-dependencies -- -k 0 > deps.log 2>&1; rc=$?
echo "deps build exit $rc"
if [ $rc -ne 0 ]; then
  grep -E "FAILED:|error:|Error" deps.log | head -40
  exit 1
fi
# The freshly built CREngine: newest patched source tree and newest libcrengine.a build dir.
# (the tarball is extracted without its top directory: sources/crengine-<hash>/ is the repo root)
src=$(ls -dt "$B"/sources/crengine-*/ 2>/dev/null | head -1 | sed 's#/$##')
lib=$(ls -t "$B"/dependencies/*/crengine/libcrengine.a 2>/dev/null | head -1)
[ -n "$src" ] && [ -f "$src/crengine/include/lvtinydom.h" ] || { echo "no CREngine source under $B/sources"; exit 1; }
[ -n "$lib" ] || { echo "no libcrengine.a under $B/dependencies"; exit 1; }
build=$(dirname "$lib")
install=$(dirname "$build")/install
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
grep -o 'crengine-[a-f0-9]*' <<< "$src" | head -1 | sed 's/^/CREngine source hash dir: /'
echo "DEPS OK"

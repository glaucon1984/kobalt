#!/bin/sh
# Sync this repository's working tree (tracked and untracked files, nothing ignored) to the docker
# VM, build and package Kobalt for PocketBook there in the pbqt-build container (static Qt in
# ~/build/pbqt/qt-pb12, prebuilt dependencies in ~/build/pbqt/kobalt-deps; see doc/building.md),
# and copy the package back to build/pocketbook/kobalt-pb.tar.gz.
# Usage: platforms/pocketbook/tools/remote-build-kobalt.sh [ssh-host] [configure|build|package|all]
set -eu
root=$(CDPATH= cd "$(dirname "$0")/../../.." && pwd)
host=${1:-docker}
what=${2:-all}
cd "$root"
commit=$(git rev-parse --short HEAD 2>/dev/null || echo "")
modified=OFF
[ -n "$commit" ] && [ -n "$(git status --porcelain --untracked-files=no 2>/dev/null)" ] && modified=ON
# The tree on the VM is replaced whole; tar keeps the file times, so the build stays incremental.
{ git ls-files -z; git ls-files -z -o --exclude-standard; } | tar --null -czf - -T - |
  ssh "$host" "rm -rf build/pbqt/kobalt && mkdir -p build/pbqt/kobalt && cd build/pbqt/kobalt && tar xzf -"
ssh "$host" "cd ~/build/pbqt && docker run --rm -u \"\$(id -u):\$(id -g)\" -e HOME=/tmp \
  -e KOBALT_COMMIT=$commit -e KOBALT_COMMIT_MODIFIED=$modified \
  -v \"\$HOME/build/pbqt:/work\" -w /work pbqt-build bash /work/kobalt/platforms/pocketbook/tools/build-kobalt-pb.sh $what"
[ "$what" = configure ] && exit 0
mkdir -p build/pocketbook
scp -q "$host:build/pbqt/kobalt-pb-build/kobalt-pb.tar.gz" build/pocketbook/kobalt-pb.tar.gz
ls -la build/pocketbook/kobalt-pb.tar.gz

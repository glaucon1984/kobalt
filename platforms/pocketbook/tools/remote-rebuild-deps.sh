#!/bin/sh
# Sync this repository's working tree to the docker VM and rebuild Kobalt's third-party libraries
# there for it (see tools/rebuild-deps.sh), refreshing ~/build/pbqt/kobalt-deps. Run after an
# upstream update that moved dependencies/manifest.json or dependencies/patches, before
# tools/remote-build-kobalt.sh.
# Usage: platforms/pocketbook/tools/remote-rebuild-deps.sh [ssh-host]
set -eu
root=$(CDPATH= cd "$(dirname "$0")/../../.." && pwd)
host=${1:-docker}
cd "$root"
{ git ls-files -z; git ls-files -z -o --exclude-standard; } | tar --null -czf - -T - |
  ssh "$host" "rm -rf build/pbqt/kobalt && mkdir -p build/pbqt/kobalt && cd build/pbqt/kobalt && tar xzf -"
ssh "$host" 'cd ~/build/pbqt && docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$HOME/build/pbqt:/work" -w /work pbqt-build bash /work/kobalt/platforms/pocketbook/tools/rebuild-deps.sh'

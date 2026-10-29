#!/bin/sh
# Historical: build the pbink plugin on its own and the hello app (the first thing that ran on the
# device) with the first cross Qt (~/build/pbqt/qt-pb). Syncs the repository to the docker VM.
# Usage: platforms/pocketbook/spike/remote-build.sh [ssh-host]
set -eu
root=$(CDPATH= cd "$(dirname "$0")/../../.." && pwd)
host=${1:-docker}
cd "$root"
{ git ls-files -z; git ls-files -z -o --exclude-standard; } | tar --null -czf - -T - |
  ssh "$host" "rm -rf build/pbqt/kobalt && mkdir -p build/pbqt/kobalt && cd build/pbqt/kobalt && tar xzf -"
ssh "$host" 'cd ~/build/pbqt && docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$HOME/build/pbqt:/work" -w /work -e JOBS=8 pbqt-build bash /work/kobalt/platforms/pocketbook/spike/build-app.sh'
mkdir -p build/pocketbook
scp -q "$host:build/pbqt/kobalt/platforms/pocketbook/spike/hello/pbqt-hello.stripped" build/pocketbook/pbqt-hello
ls -la build/pocketbook/pbqt-hello

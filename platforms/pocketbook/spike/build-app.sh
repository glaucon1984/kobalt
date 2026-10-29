#!/bin/bash
# Build the pbink plugin and the hello app with the cross Qt. Runs in the pbqt-build container
# with ~/build/pbqt mounted at /work; the repository is synced to /work/kobalt by remote-build.sh.
set -euo pipefail
QMAKE=/work/qt-pb/bin/qmake
JOBS=${JOBS:-8}
cd /work/kobalt/platforms/pocketbook/pbink
"$QMAKE" pbink.pro
make -j"$JOBS"
cd /work/kobalt/platforms/pocketbook/spike/hello
"$QMAKE" hello.pro
make -j"$JOBS"
/work/sdk/SDK-B288/usr/bin/arm-obreey-linux-gnueabi-strip -o pbqt-hello.stripped pbqt-hello
file pbqt-hello
ls -la pbqt-hello pbqt-hello.stripped
echo "APP BUILD OK"

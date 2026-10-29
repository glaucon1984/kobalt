#!/bin/bash
# Build Qt Sensors 5.15.19 against the static Qt in /work/qt-pb (Kobalt links Qt::Sensors).
set -euo pipefail
cd /work
[ -f qtsensors-5.15.19.tar.xz ] || curl -sL -o qtsensors-5.15.19.tar.xz https://download.qt.io/archive/qt/5.15/5.15.19/submodules/qtsensors-everywhere-opensource-src-5.15.19.tar.xz
[ -d qtsensors-everywhere-src-5.15.19 ] || tar xJf qtsensors-5.15.19.tar.xz
mkdir -p qtsensors-build && cd qtsensors-build
/work/qt-pb/bin/qmake ../qtsensors-everywhere-src-5.15.19
make -j"${JOBS:-8}"
make install
ls /work/qt-pb/lib/libQt5Sensors.a /work/qt-pb/lib/cmake/Qt5Sensors/Qt5SensorsConfig.cmake
echo "QTSENSORS OK"

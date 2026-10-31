#!/bin/bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
case $(uname -s) in
    Darwin) host=mac ;;
    Linux) host=linux ;;
    *) echo 'The Kobalt launcher supports Linux and macOS.' >&2; exit 2 ;;
esac
if [[ ${1:-} == --help ]]; then
    echo 'Usage: ./start.sh [--check] [CMake configuration options]'
    echo 'Builds and opens the Qt developer launcher. Reuses the desktop Qt SDK when configured.'
    echo '--check checks dependencies and Qt without building or opening the GUI.'
    echo 'For a separate SDK: ./start.sh -DCMAKE_PREFIX_PATH=/path/to/qt'
    exit 0
fi
check_only=false
if [[ ${1:-} == --check ]]; then check_only=true; shift; fi
bash "$root/platforms/launcher/check-dependencies.sh" "$host"
cd "$root/platforms/launcher"
if [[ $# -eq 0 && -z ${CMAKE_PREFIX_PATH:-} && -f "$root/build/desktop-$host/CMakeCache.txt" ]]; then
    qt_dir=$(sed -n 's/^Qt6_DIR:PATH=//p' "$root/build/desktop-$host/CMakeCache.txt")
    if [[ -n $qt_dir ]]; then set -- "-DQt6_DIR=$qt_dir" "$@"; fi
fi
if ! cmake --preset "$host" "$@"; then
    echo >&2
    echo 'Launcher configuration failed. See the CMake diagnostic above.' >&2
    echo 'If Qt is missing or too old, select a prepared native Qt 6.11+ SDK with Core, Gui and Widgets:' >&2
    echo '  ./start.sh -DCMAKE_PREFIX_PATH=/path/to/qt' >&2
    echo '  https://www.qt.io/development/download-qt-installer-oss' >&2
    echo "To replace a cached SDK: ./start.sh '-UQt6*' -DCMAKE_PREFIX_PATH=/path/to/qt" >&2
    if [[ $host == mac ]]; then echo 'For a missing Mac compiler: xcode-select --install' >&2; fi
    echo 'For compiler/build tools and additional desktop dependencies, see doc/building.md.' >&2
    exit 2
fi
if $check_only; then
    echo 'Launcher dependencies and Qt are ready. Review any optional build warnings above.'
    exit 0
fi
cmake --build --preset "$host"
exec "$root/build/launcher-$host/kobalt-launcher"

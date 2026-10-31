#!/bin/bash
# Read-only startup checks. Optional build capabilities must not block the GUI.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
host=$1
setup() {
    if [[ $host == mac ]]; then
        echo '  Install the native compiler: xcode-select --install'
        echo '  With Homebrew installed: brew install cmake ninja pkg-config make'
    else
        if [[ -r /etc/os-release ]]; then . /etc/os-release; fi
        case "${ID:-} ${ID_LIKE:-}" in
            *arch*|*cachyos*) echo '  sudo pacman -S --needed base-devel cmake ninja pkgconf git zip libpng zlib' ;;
            *debian*|*ubuntu*) echo '  sudo apt-get update'
                echo '  sudo apt-get install build-essential cmake ninja-build pkg-config make patch git zip libpng-dev zlib1g-dev' ;;
            *fedora*|*rhel*) echo '  sudo dnf install gcc-c++ cmake ninja-build pkgconf-pkg-config make patch git zip libpng-devel zlib-devel' ;;
            *) echo '  Install CMake >= 3.22, Ninja, a C/C++ compiler, make, patch, pkg-config, Git and zip through your package manager.'
                echo '  Desktop builds also need libpng and zlib development headers.' ;;
        esac
    fi
    echo '  Select a prepared native Qt 6.11+ SDK with Core, Gui and Widgets.'
    echo '  Qt installer: https://www.qt.io/development/download-qt-installer-oss'
    echo '  Then: ./start.sh -DCMAKE_PREFIX_PATH=/path/to/qt'
    echo '  Distribution Qt packages are suitable only when they provide Qt 6.11 or newer.'
    echo "  Full desktop/test requirements: $root/doc/building.md"
}
missing=''
for tool in cmake ninja; do
    if ! command -v "$tool" > /dev/null 2>&1; then missing="$missing $tool"; fi
done
if [[ -n $missing ]]; then
    echo "Cannot start the launcher. Missing required tools:$missing" >&2
    setup >&2
    exit 2
fi
version=$(cmake --version)
if [[ $version =~ ([0-9]+)\.([0-9]+) ]]; then
    if (( BASH_REMATCH[1] < 3 || (BASH_REMATCH[1] == 3 && BASH_REMATCH[2] < 22) )); then
        echo "Cannot start the launcher: CMake >= 3.22 is required (found ${BASH_REMATCH[0]})." >&2
        setup >&2
        exit 2
    fi
fi
if [[ -z ${CXX:-} ]] && ! command -v c++ >/dev/null 2>&1 && ! command -v clang++ >/dev/null 2>&1 && ! command -v g++ >/dev/null 2>&1; then
    echo 'No C++ compiler was found. Install a native compiler, or set CXX to your existing compiler.' >&2
    setup >&2
    exit 2
fi
echo 'Launcher: CMake, Ninja and a native compiler are available. Checking Qt during configuration.'
missing=''
for tool in cc git make patch pkg-config zip; do
    if ! command -v "$tool" > /dev/null 2>&1; then missing="$missing $tool"; fi
done
if [[ -n $missing ]]; then
    echo "Desktop/test builds need additional tools:$missing"
    setup
    echo 'You can still open the launcher; install these tools before using the affected build/test actions.'
fi
engine=${CONTAINER_ENGINE:-docker}
if ! command -v "$engine" >/dev/null 2>&1; then
    echo "Kobo builds unavailable: container engine '$engine' is not on PATH."
    if [[ $host == mac ]]; then
        echo '  Install Docker Desktop: https://docs.docker.com/desktop/setup/install/mac-install/'
        echo '  Open Docker Desktop, finish setup, and ensure its docker CLI is on PATH.'
    else
        echo '  Install Docker Engine for your distribution: https://docs.docker.com/engine/install/'
        echo '  Start the engine and configure access for your user, then verify: docker info'
    fi
    echo '  An alternative compatible engine can be selected with CONTAINER_ENGINE=/path/to/engine.'
    echo '  Docker is optional for desktop builds and native tests. Continuing.'
else
    cmake "-DENGINE=$engine" "-DROOT=$root" "-DHOST=$host" -P "$root/platforms/launcher/check-container.cmake"
fi

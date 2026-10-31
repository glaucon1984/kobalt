#!/bin/bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$root/build"
scratch=$(mktemp -d "$root/build/launcher-dependencies-test.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
mkdir "$scratch/missing" "$scratch/old" "$scratch/ready"
for directory in missing old ready; do
    ln -s "$(command -v dirname)" "$scratch/$directory/dirname"
done
result=0
PATH="$scratch/missing" /bin/bash "$root/platforms/launcher/check-dependencies.sh" linux > "$scratch/output" 2>&1 || result=$?
[[ $result == 2 ]]
grep -q 'Missing required tools: cmake ninja' "$scratch/output"
grep -q 'CMAKE_PREFIX_PATH' "$scratch/output"
cat > "$scratch/old/cmake" <<'OLD'
#!/bin/bash
echo 'cmake version 3.21.0'
OLD
cp "$scratch/old/cmake" "$scratch/old/ninja"
chmod +x "$scratch/old/cmake" "$scratch/old/ninja"
result=0
PATH="$scratch/old" /bin/bash "$root/platforms/launcher/check-dependencies.sh" mac > "$scratch/output" 2>&1 || result=$?
[[ $result == 2 ]]
grep -q 'CMake >= 3.22 is required' "$scratch/output"
grep -q 'xcode-select --install' "$scratch/output"
for tool in cmake ninja c++ cc git make patch pkg-config zip sh; do
    executable=$(command -v "$tool" || true)
    if [[ -n $executable ]]; then ln -s "$executable" "$scratch/ready/$tool"; fi
done
cat > "$scratch/engine" <<'ENGINE'
#!/bin/bash
set -euo pipefail
printf '%s\n' "$1" >> "$KOBALT_DEPENDENCY_PROBE_LOG"
case $1 in
    info)
        case $KOBALT_PROBE_MODE in
            stopped) echo 'permission denied while connecting to daemon' >&2; exit 1 ;;
            timeout) /bin/sleep 30 ;;
            ready) exit 0 ;;
        esac
        ;;
    image) exit 1 ;;
    *) exit 99 ;;
esac
ENGINE
chmod +x "$scratch/engine"
export KOBALT_DEPENDENCY_PROBE_LOG="$scratch/probes"
PATH="$scratch/ready" CONTAINER_ENGINE="$scratch/not-installed" /bin/bash \
    "$root/platforms/launcher/check-dependencies.sh" mac > "$scratch/output" 2>&1
grep -q 'Install Docker Desktop' "$scratch/output"
grep -q 'Continuing' "$scratch/output"
for mode in stopped ready timeout; do
    PATH="$scratch/ready" CONTAINER_ENGINE="$scratch/engine" KOBALT_PROBE_MODE=$mode /bin/bash \
        "$root/platforms/launcher/check-dependencies.sh" linux > "$scratch/output" 2>&1
    if [[ $mode == ready ]]; then
        grep -q 'qt5: prepared SDK image is missing' "$scratch/output"
        grep -q 'qt6: prepared SDK image is missing' "$scratch/output"
        grep -q 'pull ghcr.io/nicoverbruggen/kobuild@sha256:' "$scratch/output"
    else
        grep -q 'Continuing' "$scratch/output"
        grep -q 'systemctl' "$scratch/output"
    fi
done
if grep -q '^pull$' "$scratch/probes"; then echo 'Dependency checks must not pull images' >&2; exit 1; fi
# Exercise the root bootstrap's Qt failure message without changing its real cache.
mkdir -p "$scratch/export/platforms/launcher" "$scratch/qt-missing"
cp "$root/start.sh" "$scratch/export/start.sh"
cp "$root/platforms/launcher/check-dependencies.sh" "$scratch/export/platforms/launcher/"
for tool in bash dirname uname ninja c++ cc git make patch pkg-config zip sh; do
    executable=$(command -v "$tool" || true)
    if [[ -n $executable ]]; then ln -s "$executable" "$scratch/qt-missing/$tool"; fi
done
cat > "$scratch/qt-missing/cmake" <<'CMAKE'
#!/bin/bash
if [[ $1 == --version ]]; then echo 'cmake version 3.30.0'; exit 0; fi
echo 'Could not find Qt6' >&2
exit 1
CMAKE
chmod +x "$scratch/qt-missing/cmake"
result=0
PATH="$scratch/qt-missing" CONTAINER_ENGINE="$scratch/not-installed" /bin/bash \
    "$scratch/export/start.sh" --check > "$scratch/output" 2>&1 || result=$?
[[ $result == 2 ]]
grep -q 'native Qt 6.11+ SDK' "$scratch/output"
grep -q 'CMAKE_PREFIX_PATH=/path/to/qt' "$scratch/output"
grep -q 'replace a cached SDK' "$scratch/output"
echo 'PASS: missing Qt, missing tools, old CMake, Mac setup, missing engine, stopped daemon, SDK images and probe timeout guidance'

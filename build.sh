#!/bin/bash
# build.sh - Configure and compile photoscoping.
#
# Every vendored dependency script is a no-op once built (each checks its own
# stamp), so running this repeatedly is cheap. The first run is not: it builds
# FFmpeg, SDL3, freetype and harfbuzz from source, which takes a while.
set -e

usage() {
    cat <<'USAGE'
usage: ./build.sh [options]

  -j N          parallel compile jobs (default: all cores)
  --debug       configure a Debug build instead of Release
  --reconfigure re-run cmake even if build/ already exists
  --tests       run the test suite after building
  --no-app      skip packaging build/photoscoping.app
  -h, --help    show this

Run ./clean.sh to remove build output, or ./clean.sh --all to also discard the
vendored dependencies and force them to rebuild from source.
USAGE
}

JOBS="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"
BUILD_TYPE="Release"
RECONFIGURE=0
RUN_TESTS=0
PACKAGE_APP=1

while [ $# -gt 0 ]; do
    case "$1" in
        -j) JOBS="$2"; shift 2 ;;
        -j*) JOBS="${1#-j}"; shift ;;
        --debug) BUILD_TYPE="Debug"; shift ;;
        --reconfigure) RECONFIGURE=1; shift ;;
        --tests) RUN_TESTS=1; shift ;;
        --no-app) PACKAGE_APP=0; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1"; echo; usage; exit 1 ;;
    esac
done

cd "$(dirname "$0")"

echo "building photoscoping (${BUILD_TYPE}, -j${JOBS})..."

# Printed because a toolchain and SDK that disagree fail deep inside the first
# vendored configure, nowhere near the actual cause.
echo "  toolchain: ${DEVELOPER_DIR:-$(xcode-select -p)} (SDK $(xcrun --show-sdk-version))"

# Order matters: SDL3_image and SDL3_ttf both link against SDL3, so the
# vendored SDL3 has to exist before they are configured.
echo "  checking vendored SDL3..."
./vendor/build_sdl3.sh
echo "  checking vendored SDL3_image..."
./vendor/build_sdl3_image.sh
echo "  checking vendored freetype + libpng..."
./vendor/build_freetype.sh
echo "  checking vendored text stack (harfbuzz + SDL3_ttf)..."
./vendor/build_text_stack.sh

# Not optional despite CMake only warning about it: without the vendored build,
# CMake falls back to Homebrew's FFmpeg, which is --enable-gpl --enable-version3
# and would relicense photoscoping as GPLv3 if shipped.
echo "  checking vendored FFmpeg (LGPL, decode-only)..."
./vendor/build_ffmpeg.sh

# The build type is fixed at configure time and then sticky, so a cached Debug
# build would quietly stay Debug on the next plain ./build.sh -- and a plain
# ./build.sh would quietly ignore --debug. Reconfigure whenever it disagrees, so
# the flag on the command line is always what you get.
CACHED_TYPE=""
if [ -f "build/CMakeCache.txt" ]; then
    CACHED_TYPE="$(sed -n 's/^CMAKE_BUILD_TYPE:STRING=//p' build/CMakeCache.txt)"
fi
if [ ! -d "build" ] || [ "${RECONFIGURE}" = "1" ] || \
   [ "${CACHED_TYPE}" != "${BUILD_TYPE}" ]; then
    if [ -n "${CACHED_TYPE}" ] && [ "${CACHED_TYPE}" != "${BUILD_TYPE}" ]; then
        echo "  build type changing ${CACHED_TYPE} -> ${BUILD_TYPE}"
    fi
    echo "  configuring cmake..."
    cmake -B build -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
fi

echo "  compiling..."
cmake --build build -j"${JOBS}"

if [ "${RUN_TESTS}" = "1" ]; then
    echo "  running tests..."
    ( cd build && ctest --output-on-failure )
fi

# The bundle is the thing to run: the bare binary works, but only while the
# vendored libraries stay where they were built. photoscoping.app carries its
# own copies, so it can be moved.
if [ "${PACKAGE_APP}" = "1" ]; then
    echo "  packaging photoscoping.app..."
    BUNDLE_ARGS=()
    [ -d assets ] && BUNDLE_ARGS+=(--resource assets)
    [ -d fonts ] && BUNDLE_ARGS+=(--resource fonts)
    [ -f assets/icon.png ] && BUNDLE_ARGS+=(--icon assets/icon.png)
    python3 tools/bundle_app.py \
        --binary build/photoscoping \
        "${BUNDLE_ARGS[@]}" \
        --out build/photoscoping.app
    echo "done -- open build/photoscoping.app (or run ./build/photoscoping)"
else
    echo "done -- run ./build/photoscoping"
fi

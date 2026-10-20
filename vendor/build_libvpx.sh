#!/bin/bash
# build_libvpx.sh - Build libvpx's VP9 encoder for photoscoping's FFmpeg.
#
# Why: exporting the matted subject as VP9 with alpha -- the one widely
# played web format that carries transparency -- needs an encoder FFmpeg does
# not have natively. libvpx is Google's reference codec, BSD-licensed, so
# linking it keeps photoscoping LGPL-2.1.
#
# Static and position-independent: it is linked into the vendored libavcodec
# dylib (vendor/build_ffmpeg.sh), not shipped as a library of its own. VP9
# only; VP8 has no alpha worth having.
#
# The source tarball's sha256 is the one Homebrew's formula pins.
#
# Output: vendor/libvpx/{include,lib}, with a pkg-config file FFmpeg's
# configure finds.

set -euo pipefail

VPX_VERSION="1.17.0"
VPX_SHA256="1020f184046187baa2985dbde38e0691f49c44088bca7a1842b0236c6081dc0a"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${SCRIPT_DIR}/libvpx"
CACHE_DIR="${SCRIPT_DIR}/.cache"
BUILD_DIR="${SCRIPT_DIR}/.build"
TARBALL="${CACHE_DIR}/libvpx-${VPX_VERSION}.tar.gz"
SRC_DIR="${BUILD_DIR}/libvpx-${VPX_VERSION}"

if [ "${1:-}" = "--clean" ]; then
    echo "removing ${PREFIX} and ${SRC_DIR}..."
    rm -rf "${PREFIX}" "${SRC_DIR}"
    exit 0
fi

STAMP="${PREFIX}/.photoscoping-libvpx-stamp"
STAMP_VALUE="${VPX_VERSION}-$(shasum -a 256 "$0" | cut -c1-12)"
if [ -f "${STAMP}" ] && [ "$(cat "${STAMP}")" = "${STAMP_VALUE}" ]; then
    echo "vendored libvpx ${VPX_VERSION} already built at ${PREFIX}"
    exit 0
fi

mkdir -p "${CACHE_DIR}" "${BUILD_DIR}"
if [ ! -f "${TARBALL}" ]; then
    echo "downloading libvpx ${VPX_VERSION}..."
    curl -fL --retry 3 -o "${TARBALL}.tmp" \
        "https://github.com/webmproject/libvpx/archive/refs/tags/v${VPX_VERSION}.tar.gz"
    mv "${TARBALL}.tmp" "${TARBALL}"
fi
ACTUAL="$(shasum -a 256 "${TARBALL}" | awk '{print $1}')"
if [ "${ACTUAL}" != "${VPX_SHA256}" ]; then
    echo "ERROR: checksum mismatch for ${TARBALL}"
    echo "  expected ${VPX_SHA256}"
    echo "  actual   ${ACTUAL}"
    exit 1
fi

rm -rf "${SRC_DIR}"
tar -xzf "${TARBALL}" -C "${BUILD_DIR}"
cd "${SRC_DIR}"

# The target named outright: left to guess, libvpx's configure does not know
# a Darwin newer than its own release and falls back to plain C, without the
# NEON that makes VP9 encoding bearable. The newest macOS it knows, which runs
# on anything since; not "arm64-darwin-gcc", which despite the name is iOS.
case "$(uname -m)" in
    arm64) TARGET="arm64-darwin25-gcc" ;;
    x86_64) TARGET="x86_64-darwin25-gcc" ;;
    *) echo "ERROR: unsupported architecture $(uname -m)"; exit 1 ;;
esac

echo "configuring libvpx (${TARGET}, VP9, static)..."
./configure \
    --prefix="${PREFIX}" \
    --target="${TARGET}" \
    --disable-vp8 \
    --enable-vp9 \
    --enable-vp9-highbitdepth \
    --enable-static \
    --disable-shared \
    --enable-pic \
    --disable-examples \
    --disable-tools \
    --disable-docs \
    --disable-unit-tests \
    --disable-dependency-tracking

echo "compiling libvpx..."
make -j"$(sysctl -n hw.ncpu)"
rm -rf "${PREFIX}"
make install
cp LICENSE "${PREFIX}/LICENSE"
cp PATENTS "${PREFIX}/PATENTS"
echo "${STAMP_VALUE}" > "${STAMP}"
echo "libvpx ${VPX_VERSION} installed to ${PREFIX}"

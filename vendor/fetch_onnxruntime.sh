#!/bin/bash
# fetch_onnxruntime.sh - Fetch a pinned ONNX Runtime for photoscoping.
#
# ONNX Runtime runs the matting model natively: on the Mac through Core ML,
# elsewhere through DirectML, CUDA or the CPU, from one model file and one
# C++ API. tools/onnx_spike.py is where that was measured.
#
# Fetched as Microsoft's own release build rather than built here, unlike the
# rest of vendor/. A source build with the Core ML provider takes the better
# part of an hour and a toolchain of its own, and buys nothing a pinned,
# checksummed official binary does not: the release is MIT, already carries
# the Core ML provider, and is the build everyone else's bug reports are
# against. Pinning and verification are what the other scripts are for too.
#
# Output: vendor/onnxruntime/{include,lib}. Picked up by CMakeLists.txt.

set -euo pipefail

ORT_VERSION="1.31.0"

# Per platform: the release asset and its digest, as GitHub publishes it for
# the asset (https://api.github.com/repos/microsoft/onnxruntime/releases/tags/
# v1.31.0), checked again against a local download. Only the Mac one is used
# so far; the others are pinned ahead of the ports that will want them.
case "$(uname -s)-$(uname -m)" in
    Darwin-arm64)
        ORT_ASSET="onnxruntime-osx-arm64-${ORT_VERSION}.tgz"
        ORT_SHA256="1031b36dd3bdaa8d976a6130d9cf2a05e9be320b93ae804f8427b91dc3432afc"
        ;;
    Linux-x86_64)
        ORT_ASSET="onnxruntime-linux-x64-${ORT_VERSION}.tgz"
        ORT_SHA256="cc5c72baf5ae5c8238a6841f0897227be2d02826b9cf98eaf02fdefaeeb03a57"
        ;;
    MINGW*-x86_64|MSYS*-x86_64|CYGWIN*-x86_64)
        ORT_ASSET="onnxruntime-win-x64-${ORT_VERSION}.zip"
        ORT_SHA256="3958d8a44984160692c1b4ffd0de3cb48f8c39f9d34bb31e433db06e30b82195"
        ;;
    *)
        echo "ERROR: no ONNX Runtime release pinned for $(uname -s)-$(uname -m)"
        exit 1
        ;;
esac

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${SCRIPT_DIR}/onnxruntime"
CACHE_DIR="${SCRIPT_DIR}/.cache"
BUILD_DIR="${SCRIPT_DIR}/.build"
ARCHIVE="${CACHE_DIR}/${ORT_ASSET}"

if [ "${1:-}" = "--clean" ]; then
    echo "removing ${PREFIX}..."
    rm -rf "${PREFIX}"
    exit 0
fi

STAMP="${PREFIX}/.photoscoping-onnxruntime-stamp"
STAMP_VALUE="${ORT_ASSET}-$(shasum -a 256 "$0" | cut -c1-12)"
if [ -f "${STAMP}" ] && [ "$(cat "${STAMP}")" = "${STAMP_VALUE}" ]; then
    echo "vendored ONNX Runtime already fetched (${ORT_VERSION})"
    echo "(run '$0 --clean' to force a fresh one)"
    exit 0
fi

mkdir -p "${CACHE_DIR}" "${BUILD_DIR}"

if [ ! -f "${ARCHIVE}" ]; then
    echo "downloading ${ORT_ASSET}..."
    curl -fL --retry 3 -o "${ARCHIVE}.tmp" \
        "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/${ORT_ASSET}"
    mv "${ARCHIVE}.tmp" "${ARCHIVE}"
fi
GOT="$(shasum -a 256 "${ARCHIVE}" | awk '{print $1}')"
if [ "${GOT}" != "${ORT_SHA256}" ]; then
    echo "ERROR: checksum mismatch for ${ARCHIVE}"
    echo "  expected ${ORT_SHA256}"
    echo "  actual   ${GOT}"
    exit 1
fi

echo "extracting ${ORT_ASSET}..."
UNPACKED="${BUILD_DIR}/${ORT_ASSET%.*}"
rm -rf "${UNPACKED}"
case "${ORT_ASSET}" in
    *.zip) (cd "${BUILD_DIR}" && unzip -q "${ARCHIVE}") ;;
    *) tar -xzf "${ARCHIVE}" -C "${BUILD_DIR}" ;;
esac

rm -rf "${PREFIX}"
mkdir -p "${PREFIX}"
cp -R "${UNPACKED}/include" "${PREFIX}/include"
cp -R "${UNPACKED}/lib" "${PREFIX}/lib"
cp "${UNPACKED}/LICENSE" "${PREFIX}/LICENSE"
cp "${UNPACKED}/ThirdPartyNotices.txt" "${PREFIX}/ThirdPartyNotices.txt"
# Debug symbols are most of the archive and nothing links against them.
rm -rf "${PREFIX}"/lib/*.dSYM

# -------------------------------------------------------------- assertions
if [ "$(uname -s)" = "Darwin" ]; then
    ORT_LIB="${PREFIX}/lib/libonnxruntime.${ORT_VERSION}.dylib"
    [ -f "${ORT_LIB}" ] || { echo "ERROR: expected ${ORT_LIB}"; exit 1; }
    # The Core ML provider is why this build is used on the Mac at all.
    [ -f "${PREFIX}/include/coreml_provider_factory.h" ] ||
        { echo "ERROR: this ONNX Runtime has no Core ML provider"; exit 1; }
    # Linked by @rpath, which is what lets bundle_app.py carry it into the .app.
    otool -D "${ORT_LIB}" | tail -1 | grep -q "^@rpath/" ||
        { echo "ERROR: ${ORT_LIB} is not identified by @rpath"; exit 1; }
fi

echo "${STAMP_VALUE}" > "${STAMP}"
echo "ONNX Runtime ${ORT_VERSION} -> ${PREFIX}"

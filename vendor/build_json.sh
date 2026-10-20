#!/bin/bash
# build_json.sh - Vendor nlohmann/json (single header) for photoscoping.
#
# Why a JSON library at all: the fixes made to a clip's matte -- garbage
# mattes, paint, re-matted regions, each keyframed across the clip -- are kept
# beside its renders as JSON, which has to be read back exactly as written.
#
# Why this one: header-only, MIT, and the most widely deployed C++ JSON library
# there is -- which matters for a format that already exists on disk in user
# files. Compile time is the cost; there is no runtime dependency and nothing
# to ship.
#
# The single-header release asset has no published checksum, so this fetches the
# *source tarball* (whose sha256 is independently pinned by Homebrew's formula)
# and extracts the amalgamated header from it. Verified identical to the release
# asset at the time of writing.
#
# Output: vendor/json/nlohmann/json.hpp

set -euo pipefail

JSON_VERSION="3.12.0"
JSON_SHA256="4b92eb0c06d10683f7447ce9406cb97cd4b453be18d7279320f7b2f025c10187"
# sha256 of the extracted single_include/nlohmann/json.hpp, asserted after
# extraction so a tarball layout change cannot silently ship a different header.
HEADER_SHA256="aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${SCRIPT_DIR}/json"
CACHE_DIR="${SCRIPT_DIR}/.cache"
BUILD_DIR="${SCRIPT_DIR}/.build"
TARBALL="${CACHE_DIR}/json-${JSON_VERSION}.tar.gz"

if [ "${1:-}" = "--clean" ]; then
    rm -rf "${PREFIX}"
    exit 0
fi

STAMP="${PREFIX}/.photoscoping-json-stamp"
STAMP_VALUE="${JSON_VERSION}-$(shasum -a 256 "$0" | cut -c1-12)"
if [ -f "${STAMP}" ] && [ "$(cat "${STAMP}")" = "${STAMP_VALUE}" ]; then
    echo "vendored nlohmann/json already present (${JSON_VERSION})"
    exit 0
fi

mkdir -p "${CACHE_DIR}" "${BUILD_DIR}"

if [ ! -f "${TARBALL}" ]; then
    echo "downloading nlohmann/json ${JSON_VERSION}..."
    curl -fL --retry 3 -o "${TARBALL}.tmp" \
        "https://github.com/nlohmann/json/archive/refs/tags/v${JSON_VERSION}.tar.gz"
    mv "${TARBALL}.tmp" "${TARBALL}"
fi
GOT="$(shasum -a 256 "${TARBALL}" | awk '{print $1}')"
if [ "${GOT}" != "${JSON_SHA256}" ]; then
    echo "ERROR: checksum mismatch for ${TARBALL}"
    echo "  expected ${JSON_SHA256}"
    echo "  actual   ${GOT}"
    exit 1
fi

rm -rf "${BUILD_DIR}/json-${JSON_VERSION}" "${PREFIX}"
tar -xzf "${TARBALL}" -C "${BUILD_DIR}" "json-${JSON_VERSION}/single_include/nlohmann/json.hpp" \
                                        "json-${JSON_VERSION}/LICENSE.MIT"
mkdir -p "${PREFIX}/nlohmann"
cp "${BUILD_DIR}/json-${JSON_VERSION}/single_include/nlohmann/json.hpp" "${PREFIX}/nlohmann/"
cp "${BUILD_DIR}/json-${JSON_VERSION}/LICENSE.MIT" "${PREFIX}/" 2>/dev/null || true

HDR_GOT="$(shasum -a 256 "${PREFIX}/nlohmann/json.hpp" | awk '{print $1}')"
if [ "${HDR_GOT}" != "${HEADER_SHA256}" ]; then
    echo "ERROR: extracted header checksum mismatch"
    echo "  expected ${HEADER_SHA256}"
    echo "  actual   ${HDR_GOT}"
    exit 1
fi

echo "${STAMP_VALUE}" > "${STAMP}"
echo "vendored nlohmann/json ${JSON_VERSION} at ${PREFIX}"
du -sh "${PREFIX}"

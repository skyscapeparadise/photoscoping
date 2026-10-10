#!/bin/bash
# build_sdl3_image.sh - Build a minimal SDL3_image for photoscoping.
#
# photoscoping's widget set draws its icons from SVG, rasterized to a texture
# atlas at load time. Footage and stills pulled from it do NOT go through here
# (ffmpeg handles those), so the only decoder that actually matters is SVG.
#
# Homebrew's SDL3_image enables every format, dragging ~12MB of AVIF/JXL/TIFF/
# WebP decoders and their transitive deps (libaom, libhwy, brotli, lcms2,
# lzma, zstd, ...) into the app bundle to render vector icons.
#
# This build enables SVG (vendored nanosvg, no external deps) plus the
# header-only formats that cost nothing, and uses SDL_image's built-in stb
# backend for PNG/JPEG so libpng/libjpeg-turbo aren't needed either. Result:
# libSDL3_image with zero non-system dependencies beyond SDL3 itself.
#
# Output: vendor/sdl3_image/{include,lib}. Picked up by CMakeLists.txt.

set -euo pipefail

SDLIMAGE_VERSION="3.4.4"
SDLIMAGE_SHA256="29751304a13d25ac513f24305fa25b06a6edd9607718c90129b8350d35fc5573"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${SCRIPT_DIR}/sdl3_image"
CACHE_DIR="${SCRIPT_DIR}/.cache"
BUILD_DIR="${SCRIPT_DIR}/.build"
# Build against the vendored SDL3 when it exists, so the installed dylib does
# not bake in an absolute /opt/homebrew path. Falls back to whatever
# find_package locates otherwise (fine for development, not for shipping).
SDL3_PREFIX="${SCRIPT_DIR}/sdl3"
TARBALL="${CACHE_DIR}/SDL3_image-${SDLIMAGE_VERSION}.tar.gz"
SRC_DIR="${BUILD_DIR}/SDL3_image-${SDLIMAGE_VERSION}"

if [ "${1:-}" = "--clean" ]; then
    echo "removing ${PREFIX}..."
    rm -rf "${PREFIX}" "${SRC_DIR}"
    exit 0
fi

STAMP="${PREFIX}/.photoscoping-sdl3-image-stamp"
STAMP_VALUE="${SDLIMAGE_VERSION}-$(shasum -a 256 "$0" | cut -c1-12)"
if [ -f "${STAMP}" ] && [ "$(cat "${STAMP}")" = "${STAMP_VALUE}" ]; then
    echo "vendored SDL3_image ${SDLIMAGE_VERSION} already built at ${PREFIX}"
    echo "(run '$0 --clean' to force a rebuild)"
    exit 0
fi

mkdir -p "${CACHE_DIR}" "${BUILD_DIR}"

if [ ! -f "${TARBALL}" ]; then
    echo "downloading SDL3_image ${SDLIMAGE_VERSION}..."
    curl -fL --retry 3 -o "${TARBALL}.tmp" \
        "https://github.com/libsdl-org/SDL_image/releases/download/release-${SDLIMAGE_VERSION}/SDL3_image-${SDLIMAGE_VERSION}.tar.gz"
    mv "${TARBALL}.tmp" "${TARBALL}"
fi

echo "verifying checksum..."
ACTUAL="$(shasum -a 256 "${TARBALL}" | awk '{print $1}')"
if [ "${ACTUAL}" != "${SDLIMAGE_SHA256}" ]; then
    echo "ERROR: checksum mismatch for ${TARBALL}"
    echo "  expected ${SDLIMAGE_SHA256}"
    echo "  actual   ${ACTUAL}"
    exit 1
fi

echo "extracting..."
rm -rf "${SRC_DIR}"
tar -xzf "${TARBALL}" -C "${BUILD_DIR}"

# The four formats below are the only ones with external dependencies, and
# each pulls in a subtree: AVIF -> libavif, libaom, libdav1d; JXL -> libjxl,
# libhwy, brotli, lcms2; TIF -> libtiff, liblzma, libzstd; WEBP -> libwebp,
# libsharpyuv. All off. BACKEND_STB=ON keeps PNG/JPG working via SDL_image's
# bundled stb_image rather than linking libpng/libjpeg-turbo.
echo "configuring (SVG + dependency-free formats only)..."
cmake -S "${SRC_DIR}" -B "${SRC_DIR}/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    -DCMAKE_PREFIX_PATH="${SDL3_PREFIX}" \
    -DBUILD_SHARED_LIBS=ON \
    -DSDLIMAGE_SAMPLES=OFF \
    -DSDLIMAGE_TESTS=OFF \
    -DSDLIMAGE_VENDORED=OFF \
    -DSDLIMAGE_BACKEND_STB=ON \
    -DSDLIMAGE_AVIF=OFF \
    -DSDLIMAGE_JXL=OFF \
    -DSDLIMAGE_TIF=OFF \
    -DSDLIMAGE_WEBP=OFF \
    -DSDLIMAGE_SVG=ON \
    -DSDLIMAGE_PNG=ON \
    -DSDLIMAGE_JPG=ON

echo "compiling..."
cmake --build "${SRC_DIR}/build" -j"$(sysctl -n hw.ncpu)"

echo "installing to ${PREFIX}..."
rm -rf "${PREFIX}"
cmake --install "${SRC_DIR}/build"

cp "${SRC_DIR}/LICENSE.txt" "${PREFIX}/LICENSE.txt" 2>/dev/null || true
echo "${STAMP_VALUE}" > "${STAMP}"

# The whole point is dropping the dependency subtree, so fail if any of it
# survived rather than discovering it in the app bundle later.
echo
echo "verifying no heavy image dependencies leaked in..."
LEAKED="$(otool -L "${PREFIX}/lib/libSDL3_image.0.dylib" \
    | grep -oE "lib(avif|aom|jxl|hwy|brotli[a-z]*|lcms2|tiff|lzma|zstd|webp[a-z]*|sharpyuv|png16|jpeg)\.[0-9.]*dylib" \
    | sort -u || true)"
if [ -n "${LEAKED}" ]; then
    echo "ERROR: libSDL3_image still links:"
    echo "${LEAKED}" | sed 's/^/    /'
    exit 1
fi
echo "  clean -- links only SDL3 and system libraries"

# libSDL3_image is installed with an @rpath/libSDL3.0.dylib load command and,
# unlike SDL3_ttf, no LC_RPATH at all. Against Homebrew's SDL3 that happened to
# resolve via the absolute install_name; against the vendored one it does not.
# See build_text_stack.sh for why two entries are needed.
install_name_tool -add_rpath "@loader_path" "${PREFIX}/lib/libSDL3_image.0.dylib" 2>/dev/null || true
install_name_tool -add_rpath "@loader_path/../../sdl3/lib" "${PREFIX}/lib/libSDL3_image.0.dylib" 2>/dev/null || true
codesign --force --sign - "${PREFIX}/lib/libSDL3_image.0.dylib" 2>/dev/null || true

# An unresolvable @rpath links cleanly and only fails at launch, so assert the
# real resolution path rather than the load commands. This check did not exist
# before SDL3 was vendored, which is exactly why the missing LC_RPATH above
# went unnoticed.
echo
echo "verifying SDL3_image actually loads..."
cat > "${BUILD_DIR}/imgloadchk.c" <<'EOF'
#include <stdio.h>
#include <dlfcn.h>
int main(int argc, char **argv) {
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) { printf("  FAIL: %s\n", dlerror()); return 1; }
    if (!dlsym(h, "IMG_Load")) { printf("  FAIL: IMG_Load not found\n"); return 1; }
    printf("  dlopen(libSDL3_image) + IMG_Load resolved: OK\n");
    return 0;
}
EOF
clang "${BUILD_DIR}/imgloadchk.c" -o "${BUILD_DIR}/imgloadchk"
if ! "${BUILD_DIR}/imgloadchk" "${PREFIX}/lib/libSDL3_image.0.dylib"; then
    echo "ERROR: vendored SDL3_image cannot be loaded. Refusing to continue."
    exit 1
fi

echo
echo "vendored SDL3_image ${SDLIMAGE_VERSION} built at ${PREFIX}"
du -sh "${PREFIX}/lib"

#!/bin/bash
# build_freetype.sh - Build libpng + freetype for photoscoping.
#
# freetype was the last non-system dependency in the graph: harfbuzz links it,
# and taking it from Homebrew baked an absolute /opt/homebrew path into the
# bundle. Everything else was already vendored.
#
# libpng is built here too because it is freetype's only non-system dependency
# (zlib and bzip2 both come from the macOS dyld shared cache). It could be
# dropped -- FT_DISABLE_PNG=ON -- but PNG is how colour bitmap glyphs are
# stored (Apple Color Emoji's sbix table), so dropping it silently removes
# emoji from any clip name or note a user types; not a trade worth ~250KB.
# FT_REQUIRE_PNG below makes losing it a build failure rather than a silent
# regression.
#
# One shared prefix on purpose, same reason as build_text_stack.sh: libfreetype
# resolves libpng via @rpath, so the two dylibs must be siblings.
#
# NOT built here: brotli (WOFF2 web fonts -- photoscoping loads local font files)
# and harfbuzz-in-freetype (freetype can use harfbuzz for autohinting, but
# harfbuzz depends on freetype; enabling it creates a circular build).
#
# Output: vendor/freetype/{include,lib}. Consumed by build_text_stack.sh.

set -euo pipefail

PNG_VERSION="1.6.58"
# Cross-checked against Homebrew's own pinned formula checksums rather than a
# single download of our own.
PNG_SHA256="28eb403f51f0f7405249132cecfe82ea5c0ef97f1b32c5a65828814ae0d34775"
FT_VERSION="2.14.3"
FT_SHA256="36bc4f1cc413335368ee656c42afca65c5a3987e8768cc28cf11ba775e785a5f"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${SCRIPT_DIR}/freetype"
CACHE_DIR="${SCRIPT_DIR}/.cache"
BUILD_DIR="${SCRIPT_DIR}/.build"
PNG_SRC="${BUILD_DIR}/libpng-${PNG_VERSION}"
FT_SRC="${BUILD_DIR}/freetype-${FT_VERSION}"

if [ "${1:-}" = "--clean" ]; then
    echo "removing ${PREFIX}..."
    rm -rf "${PREFIX}" "${PNG_SRC}" "${FT_SRC}"
    exit 0
fi

STAMP="${PREFIX}/.photoscoping-freetype-stamp"
STAMP_VALUE="${PNG_VERSION}-${FT_VERSION}-$(shasum -a 256 "$0" | cut -c1-12)"
if [ -f "${STAMP}" ] && [ "$(cat "${STAMP}")" = "${STAMP_VALUE}" ]; then
    echo "vendored freetype already built (libpng ${PNG_VERSION}, freetype ${FT_VERSION})"
    echo "(run '$0 --clean' to force a rebuild)"
    exit 0
fi

mkdir -p "${CACHE_DIR}" "${BUILD_DIR}"

fetch() {
    url="$1"; out="$2"; want="$3"
    if [ ! -f "${out}" ]; then
        echo "downloading $(basename "${out}")..."
        curl -fL --retry 3 -o "${out}.tmp" "${url}"
        mv "${out}.tmp" "${out}"
    fi
    got="$(shasum -a 256 "${out}" | awk '{print $1}')"
    if [ "${got}" != "${want}" ]; then
        echo "ERROR: checksum mismatch for ${out}"
        echo "  expected ${want}"
        echo "  actual   ${got}"
        exit 1
    fi
}

fetch "https://downloads.sourceforge.net/project/libpng/libpng16/${PNG_VERSION}/libpng-${PNG_VERSION}.tar.xz" \
      "${CACHE_DIR}/libpng-${PNG_VERSION}.tar.xz" "${PNG_SHA256}"
fetch "https://downloads.sourceforge.net/project/freetype/freetype2/${FT_VERSION}/freetype-${FT_VERSION}.tar.xz" \
      "${CACHE_DIR}/freetype-${FT_VERSION}.tar.xz" "${FT_SHA256}"

rm -rf "${PREFIX}"

# ------------------------------------------------------------------ libpng
echo "extracting libpng..."
rm -rf "${PNG_SRC}"
tar -xf "${CACHE_DIR}/libpng-${PNG_VERSION}.tar.xz" -C "${BUILD_DIR}"

echo "configuring libpng (shared only, no tools/tests)..."
cmake -S "${PNG_SRC}" -B "${PNG_SRC}/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    -DBUILD_SHARED_LIBS=ON \
    -DPNG_SHARED=ON \
    -DPNG_STATIC=OFF \
    -DPNG_TESTS=OFF \
    -DPNG_TOOLS=OFF \
    -DPNG_FRAMEWORK=OFF

echo "compiling libpng..."
cmake --build "${PNG_SRC}/build" -j"$(sysctl -n hw.ncpu)"
cmake --install "${PNG_SRC}/build"
cp "${PNG_SRC}/LICENSE" "${PREFIX}/LICENSE.libpng" 2>/dev/null || true

# ---------------------------------------------------------------- freetype
echo "extracting freetype..."
rm -rf "${FT_SRC}"
tar -xf "${CACHE_DIR}/freetype-${FT_VERSION}.tar.xz" -C "${BUILD_DIR}"

echo "configuring freetype against the vendored libpng..."
cmake -S "${FT_SRC}" -B "${FT_SRC}/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    -DCMAKE_PREFIX_PATH="${PREFIX}" \
    -DBUILD_SHARED_LIBS=ON \
    -DFT_REQUIRE_PNG=ON \
    -DFT_REQUIRE_ZLIB=ON \
    -DFT_DISABLE_BROTLI=ON \
    -DFT_DISABLE_HARFBUZZ=ON

echo "compiling freetype..."
cmake --build "${FT_SRC}/build" -j"$(sysctl -n hw.ncpu)"
cmake --install "${FT_SRC}/build"
cp "${FT_SRC}/LICENSE.TXT" "${PREFIX}/LICENSE.freetype" 2>/dev/null || true

# libfreetype resolves libpng via @rpath and CMake strips build-tree rpaths on
# install. Both live in ${PREFIX}/lib, and in a packaged .app both land in
# Contents/Frameworks/, so @loader_path covers both layouts.
FT_LIB="${PREFIX}/lib/libfreetype.6.dylib"
install_name_tool -add_rpath "@loader_path" "${FT_LIB}" 2>/dev/null || true
codesign --force --sign - "${FT_LIB}" 2>/dev/null || true

# -------------------------------------------------------------- assertions
for f in "${FT_LIB}" "${PREFIX}/lib/libpng16.16.dylib"; do
    if [ ! -f "${f}" ]; then
        echo "ERROR: expected build output missing: ${f}"
        exit 1
    fi
done

echo
echo "verifying no non-system dependencies..."
LEAK="$(otool -L "${FT_LIB}" "${PREFIX}/lib/libpng16.16.dylib" \
    | grep -E '/opt/homebrew|/usr/local' || true)"
if [ -n "${LEAK}" ]; then
    echo "ERROR: vendored freetype still links non-system libraries:"
    echo "${LEAK}" | sed 's/^/    /'
    exit 1
fi
echo "  clean -- vendored libpng plus system libraries only"

# FT_REQUIRE_PNG makes a missing libpng a configure error, but it does not
# prove the built dylib actually links it -- assert that directly, since losing
# it would silently drop colour emoji rather than fail anything.
echo
echo "verifying colour-bitmap (PNG) support is linked in..."
if ! otool -L "${FT_LIB}" | grep -q 'libpng16'; then
    echo "ERROR: libfreetype does not link libpng -- colour emoji would not render."
    exit 1
fi
echo "  libfreetype links libpng16: OK"

echo
echo "verifying freetype actually loads..."
cat > "${BUILD_DIR}/ftloadchk.c" <<'EOF'
#include <stdio.h>
#include <dlfcn.h>
int main(int argc, char **argv) {
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) { printf("  FAIL: %s\n", dlerror()); return 1; }
    if (!dlsym(h, "FT_Init_FreeType")) { printf("  FAIL: FT_Init_FreeType not found\n"); return 1; }
    printf("  dlopen(libfreetype) + FT_Init_FreeType resolved: OK\n");
    return 0;
}
EOF
clang "${BUILD_DIR}/ftloadchk.c" -o "${BUILD_DIR}/ftloadchk"
if ! "${BUILD_DIR}/ftloadchk" "${FT_LIB}"; then
    echo "ERROR: vendored freetype cannot be loaded. Refusing to continue."
    exit 1
fi

echo "${STAMP_VALUE}" > "${STAMP}"
echo
echo "vendored freetype ${FT_VERSION} + libpng ${PNG_VERSION} built at ${PREFIX}"
du -sh "${PREFIX}/lib"

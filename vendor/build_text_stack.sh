#!/bin/bash
# build_text_stack.sh - Build minimal HarfBuzz + SDL3_ttf for photoscoping.
#
# Homebrew's harfbuzz links glib (which drags in libpcre2 and libintl) and
# graphite2: ~2.2MB in the app bundle. Neither is needed here.
#   * glib only provides harfbuzz's Unicode property lookups. With it off,
#     harfbuzz uses its own embedded UCD tables (hb_ucd_get_unicode_funcs in
#     src/hb-unicode.cc), which is the upstream default and fully functional.
#   * graphite2 only matters for SIL Graphite fonts. Monaspace and every other
#     font photoscoping ships or is likely to load is OpenType.
#
# HarfBuzz itself is KEPT: the widget set renders proportional UI text, with
# kerning, elide and word wrap, which needs real shaping and accurate advance
# widths, not per-glyph blitting on a fixed cell grid. Do NOT build SDL3_ttf
# with -DSDLTTF_HARFBUZZ=OFF.
#
# freetype is vendored too (vendor/build_freetype.sh), together with libpng.
# It used to come from Homebrew on the grounds that it is small and carries no
# license problem -- both true, but it was the last thing baking an absolute
# /opt/homebrew path into the bundle, which cannot ship.
#
# Output: vendor/textstack. Picked up by CMakeLists.txt.

set -euo pipefail

HB_VERSION="14.3.1"
HB_SHA256="9dae9538aae2ffdf70cec31f2c27bf68e2aaeeae3112688467697d5faf6194f7"
TTF_VERSION="3.2.2"
TTF_SHA256="63547d58d0185c833213885b635a2c0548201cc8f301e6587c0be1a67e1e045d"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
# One shared prefix on purpose. HarfBuzz installs with an install_name of
# @rpath/libharfbuzz.dylib, so libSDL3_ttf can only resolve it via an
# @loader_path rpath -- which means the two dylibs must be siblings.
PREFIX="${SCRIPT_DIR}/textstack"
HB_PREFIX="${PREFIX}"
TTF_PREFIX="${PREFIX}"
CACHE_DIR="${SCRIPT_DIR}/.cache"
BUILD_DIR="${SCRIPT_DIR}/.build"
# See build_sdl3_image.sh -- same reason.
SDL3_PREFIX="${SCRIPT_DIR}/sdl3"
FREETYPE_PREFIX="${SCRIPT_DIR}/freetype"

if [ "${1:-}" = "--clean" ]; then
    echo "removing ${PREFIX}..."
    rm -rf "${PREFIX}"
    rm -rf "${BUILD_DIR}/harfbuzz-${HB_VERSION}" "${BUILD_DIR}/SDL3_ttf-${TTF_VERSION}"
    exit 0
fi

STAMP="${TTF_PREFIX}/.photoscoping-text-stack-stamp"
STAMP_VALUE="${HB_VERSION}-${TTF_VERSION}-$(shasum -a 256 "$0" | cut -c1-12)"
if [ -f "${STAMP}" ] && [ "$(cat "${STAMP}")" = "${STAMP_VALUE}" ]; then
    echo "vendored text stack already built (harfbuzz ${HB_VERSION}, SDL3_ttf ${TTF_VERSION})"
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

fetch "https://github.com/harfbuzz/harfbuzz/releases/download/${HB_VERSION}/harfbuzz-${HB_VERSION}.tar.xz" \
      "${CACHE_DIR}/harfbuzz-${HB_VERSION}.tar.xz" "${HB_SHA256}"
fetch "https://github.com/libsdl-org/SDL_ttf/releases/download/release-${TTF_VERSION}/SDL3_ttf-${TTF_VERSION}.tar.gz" \
      "${CACHE_DIR}/SDL3_ttf-${TTF_VERSION}.tar.gz" "${TTF_SHA256}"

# ---------------------------------------------------------------- harfbuzz
HB_SRC="${BUILD_DIR}/harfbuzz-${HB_VERSION}"
echo "extracting harfbuzz..."
rm -rf "${HB_SRC}"
tar -xf "${CACHE_DIR}/harfbuzz-${HB_VERSION}.tar.xz" -C "${BUILD_DIR}"

echo "configuring harfbuzz (freetype only, no glib/graphite2/icu/cairo)..."
cmake -S "${HB_SRC}" -B "${HB_SRC}/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${HB_PREFIX}" \
    -DCMAKE_PREFIX_PATH="${FREETYPE_PREFIX}" \
    -DBUILD_SHARED_LIBS=ON \
    -DHB_HAVE_FREETYPE=ON \
    -DHB_HAVE_GLIB=OFF \
    -DHB_HAVE_GRAPHITE2=OFF \
    -DHB_HAVE_ICU=OFF \
    -DHB_HAVE_CAIRO=OFF \
    -DHB_HAVE_GOBJECT=OFF \
    -DHB_HAVE_INTROSPECTION=OFF \
    -DHB_BUILD_UTILS=OFF \
    -DHB_BUILD_SUBSET=OFF \
    -DHB_BUILD_RASTER=OFF \
    -DHB_BUILD_VECTOR=OFF \
    -DHB_BUILD_GPU=OFF

echo "compiling harfbuzz..."
cmake --build "${HB_SRC}/build" -j"$(sysctl -n hw.ncpu)"
rm -rf "${PREFIX}"
cmake --install "${HB_SRC}/build"
cp "${HB_SRC}/COPYING" "${HB_PREFIX}/COPYING" 2>/dev/null || true

# --------------------------------------------------------------- SDL3_ttf
TTF_SRC="${BUILD_DIR}/SDL3_ttf-${TTF_VERSION}"
echo "extracting SDL3_ttf..."
rm -rf "${TTF_SRC}"
tar -xzf "${CACHE_DIR}/SDL3_ttf-${TTF_VERSION}.tar.gz" -C "${BUILD_DIR}"

echo "configuring SDL3_ttf against the vendored harfbuzz..."
cmake -S "${TTF_SRC}" -B "${TTF_SRC}/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${TTF_PREFIX}" \
    -DCMAKE_PREFIX_PATH="${HB_PREFIX};${SDL3_PREFIX};${FREETYPE_PREFIX}" \
    -DBUILD_SHARED_LIBS=ON \
    -DSDLTTF_VENDORED=OFF \
    -DSDLTTF_HARFBUZZ=ON \
    -DSDLTTF_PLUTOSVG=OFF \
    -DSDLTTF_SAMPLES=OFF \
    -DSDLTTF_INSTALL=ON

echo "compiling SDL3_ttf..."
cmake --build "${TTF_SRC}/build" -j"$(sysctl -n hw.ncpu)"
cmake --install "${TTF_SRC}/build"

# CMake strips build-tree rpaths on install, leaving libSDL3_ttf with
# @rpath/libharfbuzz.dylib and @rpath/libSDL3.0.dylib load commands and no
# rpath to satisfy either. Two entries are needed because the two libraries
# sit in different places depending on the layout:
#   @loader_path            -- harfbuzz is a sibling here, and in a packaged
#                              .app every dylib lands in Contents/Frameworks/
#                              together, so this resolves SDL3 there too.
#   @loader_path/../../sdl3/lib -- the development tree, where SDL3 lives in
#                              its own vendored prefix (vendor/sdl3/lib).
# Without the second one this links fine and dies at dlopen; the load check
# below is what catches that.
install_name_tool -add_rpath "@loader_path" "${TTF_PREFIX}/lib/libSDL3_ttf.0.dylib" 2>/dev/null || true
install_name_tool -add_rpath "@loader_path/../../sdl3/lib" "${TTF_PREFIX}/lib/libSDL3_ttf.0.dylib" 2>/dev/null || true
# ... and freetype, which SDL3_ttf links directly rather than only via harfbuzz.
install_name_tool -add_rpath "@loader_path/../../freetype/lib" "${TTF_PREFIX}/lib/libSDL3_ttf.0.dylib" 2>/dev/null || true

# harfbuzz has the same problem one level down: it links @rpath/libfreetype.6.dylib,
# which now lives in the vendored freetype prefix rather than in Homebrew.
install_name_tool -add_rpath "@loader_path" "${HB_PREFIX}/lib/libharfbuzz.dylib" 2>/dev/null || true
install_name_tool -add_rpath "@loader_path/../../freetype/lib" "${HB_PREFIX}/lib/libharfbuzz.dylib" 2>/dev/null || true
codesign --force --sign - "${HB_PREFIX}/lib/libharfbuzz.dylib" 2>/dev/null || true
codesign --force --sign - "${TTF_PREFIX}/lib/libSDL3_ttf.0.dylib" 2>/dev/null || true
cp "${TTF_SRC}/LICENSE.txt" "${TTF_PREFIX}/LICENSE.txt" 2>/dev/null || true

# -------------------------------------------------------------- assertions
HB_LIB="${HB_PREFIX}/lib/libharfbuzz.dylib"
TTF_LIB="${TTF_PREFIX}/lib/libSDL3_ttf.0.dylib"

echo
echo "verifying glib/graphite2 are gone..."
# Check the files exist first: otool failing on a missing path would otherwise
# be swallowed and read as "no leaks found".
for f in "${HB_LIB}" "${TTF_LIB}"; do
    if [ ! -f "${f}" ]; then
        echo "ERROR: expected build output missing: ${f}"
        exit 1
    fi
done
LEAK="$(otool -L "${HB_LIB}" "${TTF_LIB}" \
    | grep -oE "lib(glib-2\.0|gobject-2\.0|pcre2-8|intl|graphite2|icuuc)\.[0-9.]*dylib|/opt/homebrew[^ ]*|/usr/local/[^ ]*" | sort -u || true)"
if [ -n "${LEAK}" ]; then
    echo "ERROR: the vendored text stack still links:"
    echo "${LEAK}" | sed 's/^/    /'
    exit 1
fi
echo "  clean -- harfbuzz links only the vendored freetype"

# Dropping glib swaps harfbuzz's Unicode backend for its built-in UCD tables.
# That is upstream's default, but it is exactly the kind of change that could
# silently degrade shaping, so assert the real behaviour rather than the config.
#
# The property that matters for proportional UI text is the property that actually matters is GPOS kerning and correct advance
# widths -- get those wrong and every elide, every centred label and every
# measured layout box is subtly off. So that is what is asserted here.
echo
echo "verifying Unicode backend and OpenType shaping..."
cat > "${BUILD_DIR}/shapechk.c" <<'EOF'
#include <stdio.h>
#include <hb.h>

/* 1. The Unicode property lookups glib used to provide, now served by
      harfbuzz's embedded UCD tables. If this regressed, shaping of anything
      non-trivial (scripts, marks, bidi mirroring) would quietly break. */
static int check_unicode(void) {
    hb_unicode_funcs_t *u = hb_unicode_funcs_get_default();
    int bad = 0;
    if (hb_unicode_general_category(u, 'A') != HB_UNICODE_GENERAL_CATEGORY_UPPERCASE_LETTER) {
        printf("  FAIL: general_category('A')\n"); bad = 1; }
    if (hb_unicode_script(u, 0x05D0) != HB_SCRIPT_HEBREW) {
        printf("  FAIL: script(U+05D0) should be Hebrew\n"); bad = 1; }
    if (hb_unicode_script(u, 0x3042) != HB_SCRIPT_HIRAGANA) {
        printf("  FAIL: script(U+3042) should be Hiragana\n"); bad = 1; }
    if (hb_unicode_mirroring(u, '(') != ')') {
        printf("  FAIL: mirroring('(')\n"); bad = 1; }
    if (hb_unicode_combining_class(u, 0x0301) != 230) {
        printf("  FAIL: combining_class(U+0301) should be 230\n"); bad = 1; }
    if (!bad) printf("  unicode backend (built-in UCD): OK\n");
    return bad;
}

/* 2. Plain ASCII must map one glyph per codepoint -- a sanity check that the
      shaper is wired up and returning sensible output for the trivial case. */
static int check_ascii(hb_face_t *face) {
    hb_font_t *f = hb_font_create(face); hb_font_set_scale(f, 1000, 1000);
    hb_buffer_t *b = hb_buffer_create();
    hb_buffer_add_utf8(b, "abcdef", -1, 0, -1);
    hb_buffer_guess_segment_properties(b);
    hb_shape(f, b, NULL, 0);
    unsigned n = hb_buffer_get_length(b);
    hb_buffer_destroy(b); hb_font_destroy(f);
    printf(n == 6 ? "  ascii 1:1 shaping: OK\n" : "  FAIL: ascii shaped to %u glyphs\n", n);
    return n != 6;
}

/* 3. Real GPOS kerning, proving the positioning half of the shaping engine
      works. Shaped advances for a kerned pair must differ from the sum of the
      glyphs' nominal advances. Several classic pairs are tried because which
      ones a given face kerns is a design decision, not a guarantee -- one hit
      is enough to prove GPOS is being applied. */
static int check_kerning(hb_face_t *face) {
    const char *pairs[8] = { "AV", "To", "Yo", "VA", "Ta", "AW", "P.", "F," };
    int kerned = 0;
    for (int t = 0; t < 8; t++) {
        hb_font_t *f = hb_font_create(face); hb_font_set_scale(f, 1000, 1000);
        hb_buffer_t *b = hb_buffer_create();
        hb_buffer_add_utf8(b, pairs[t], -1, 0, -1);
        hb_buffer_guess_segment_properties(b);
        hb_shape(f, b, NULL, 0);
        unsigned n = hb_buffer_get_length(b);
        hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(b, NULL);
        hb_glyph_info_t *g = hb_buffer_get_glyph_infos(b, NULL);
        int shaped = 0, nominal = 0;
        for (unsigned i = 0; i < n; i++) {
            shaped += pos[i].x_advance;
            nominal += hb_font_get_glyph_h_advance(f, g[i].codepoint);
        }
        if (n == 2 && shaped != nominal) kerned++;
        hb_buffer_destroy(b); hb_font_destroy(f);
    }
    printf(kerned > 0 ? "  gpos kerning applied: %d/8 pairs OK\n"
                      : "  FAIL: no gpos kerning on any of %d pairs\n",
           kerned > 0 ? kerned : 8);
    return kerned == 0;
}

int main(int argc, char **argv) {
    hb_blob_t *blob = hb_blob_create_from_file(argv[1]);
    if (!blob || hb_blob_get_length(blob) == 0) { printf("  cannot read %s\n", argv[1]); return 1; }
    hb_face_t *face = hb_face_create(blob, 0);
    if (hb_face_get_glyph_count(face) == 0) { printf("  no glyphs in face\n"); return 1; }
    return check_unicode() | check_ascii(face) | check_kerning(face);
}
EOF
clang "${BUILD_DIR}/shapechk.c" -o "${BUILD_DIR}/shapechk" \
    -I"${HB_PREFIX}/include/harfbuzz" \
    "${HB_LIB}" -Wl,-rpath,"${HB_PREFIX}/lib"
if ! "${BUILD_DIR}/shapechk" "${REPO_ROOT}/fonts/MonaSans-VariableFont_wdth,wght.ttf"; then
    echo "ERROR: vendored harfbuzz does not shape correctly. Refusing to continue."
    exit 1
fi

# The failure this catches is a silently unloadable library: an @rpath load
# command with no matching LC_RPATH resolves fine at link time and dies at
# launch. dlopen exercises the real resolution path.
echo
echo "verifying the text stack actually loads..."
cat > "${BUILD_DIR}/loadchk.c" <<'EOF'
#include <stdio.h>
#include <dlfcn.h>
int main(int argc, char **argv) {
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) { printf("  FAIL: %s\n", dlerror()); return 1; }
    if (!dlsym(h, "TTF_Init")) { printf("  FAIL: TTF_Init not found\n"); return 1; }
    printf("  dlopen(libSDL3_ttf) + TTF_Init resolved: OK\n");
    return 0;
}
EOF
clang "${BUILD_DIR}/loadchk.c" -o "${BUILD_DIR}/loadchk"
if ! "${BUILD_DIR}/loadchk" "${TTF_PREFIX}/lib/libSDL3_ttf.0.dylib"; then
    echo "ERROR: vendored SDL3_ttf cannot be loaded. Refusing to continue."
    exit 1
fi

echo "${STAMP_VALUE}" > "${STAMP}"
echo
echo "vendored text stack built:"
du -sh "${PREFIX}/lib"

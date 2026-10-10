#!/bin/bash
# build_sdl3.sh - Build a pinned SDL3 for photoscoping.
#
# Why vendor SDL3 at all, given it is zlib-licensed and carries no license
# problem the way ffmpeg does:
#
#   * Version pinning. Homebrew tracks whatever is current (3.4.12 at the time
#     this was written); photoscoping's renderer is built directly on SDL_GPU's
#     swapchain-composition API, which is young enough that "whatever the
#     machine happens to have" is not a sound foundation.
#   * Self-contained bundles. Linking Homebrew's dylib bakes an absolute
#     /opt/homebrew path into the binary, which cannot ship.
#   * Reproducibility. Every machine and CI runner builds against the same
#     source tarball, verified by checksum.
#
# What is NOT done here: stripping subsystems. It is tempting -- photoscoping
# uses SDL_GPU and never SDL_Renderer -- but SDL3_ttf and SDL3_image both build
# against SDL_RENDER, so disabling it breaks the rest of the vendored stack for
# the sake of a fraction of ~2.5MB. Tests, examples and the static library are
# turned off (pure build hygiene, no functional risk); the subsystems are left
# at upstream defaults deliberately.
#
# Output: vendor/sdl3/{include,lib}. Picked up by CMakeLists.txt, and by
# build_sdl3_image.sh / build_text_stack.sh via CMAKE_PREFIX_PATH.

set -euo pipefail

SDL_VERSION="3.4.16"
# Cross-checked against the digest GitHub publishes for the release asset
# (https://api.github.com/repos/libsdl-org/SDL/releases/tags/release-3.4.16),
# not just against a local download.
SDL_SHA256="7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${SCRIPT_DIR}/sdl3"
CACHE_DIR="${SCRIPT_DIR}/.cache"
BUILD_DIR="${SCRIPT_DIR}/.build"
TARBALL="${CACHE_DIR}/SDL3-${SDL_VERSION}.tar.gz"
SRC_DIR="${BUILD_DIR}/SDL3-${SDL_VERSION}"

if [ "${1:-}" = "--clean" ]; then
    echo "removing ${PREFIX}..."
    rm -rf "${PREFIX}" "${SRC_DIR}"
    exit 0
fi

STAMP="${PREFIX}/.photoscoping-sdl3-stamp"
STAMP_VALUE="${SDL_VERSION}-$(shasum -a 256 "$0" | cut -c1-12)"
if [ -f "${STAMP}" ] && [ "$(cat "${STAMP}")" = "${STAMP_VALUE}" ]; then
    echo "vendored SDL3 already built (${SDL_VERSION})"
    echo "(run '$0 --clean' to force a rebuild)"
    exit 0
fi

mkdir -p "${CACHE_DIR}" "${BUILD_DIR}"

if [ ! -f "${TARBALL}" ]; then
    echo "downloading SDL3-${SDL_VERSION}.tar.gz..."
    curl -fL --retry 3 -o "${TARBALL}.tmp" \
        "https://github.com/libsdl-org/SDL/releases/download/release-${SDL_VERSION}/SDL3-${SDL_VERSION}.tar.gz"
    mv "${TARBALL}.tmp" "${TARBALL}"
fi
GOT="$(shasum -a 256 "${TARBALL}" | awk '{print $1}')"
if [ "${GOT}" != "${SDL_SHA256}" ]; then
    echo "ERROR: checksum mismatch for ${TARBALL}"
    echo "  expected ${SDL_SHA256}"
    echo "  actual   ${GOT}"
    exit 1
fi

echo "extracting SDL3..."
rm -rf "${SRC_DIR}"
tar -xzf "${TARBALL}" -C "${BUILD_DIR}"

echo "configuring SDL3 (shared only, no tests/examples)..."
cmake -S "${SRC_DIR}" -B "${SRC_DIR}/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    -DBUILD_SHARED_LIBS=ON \
    -DSDL_SHARED=ON \
    -DSDL_STATIC=OFF \
    -DSDL_TEST_LIBRARY=OFF \
    -DSDL_TESTS=OFF \
    -DSDL_EXAMPLES=OFF \
    -DSDL_INSTALL_TESTS=OFF

echo "compiling SDL3..."
cmake --build "${SRC_DIR}/build" -j"$(sysctl -n hw.ncpu)"
rm -rf "${PREFIX}"
cmake --install "${SRC_DIR}/build"
cp "${SRC_DIR}/LICENSE.txt" "${PREFIX}/LICENSE.txt" 2>/dev/null || true

# -------------------------------------------------------------- assertions
SDL_LIB="${PREFIX}/lib/libSDL3.0.dylib"
if [ ! -f "${SDL_LIB}" ]; then
    echo "ERROR: expected build output missing: ${SDL_LIB}"
    exit 1
fi

echo
echo "verifying no non-system dependencies..."
# SDL3 should link only macOS frameworks and libSystem. Anything from
# /opt/homebrew or /usr/local here would defeat the point of vendoring.
LEAK="$(otool -L "${SDL_LIB}" | tail -n +2 | grep -E '/opt/homebrew|/usr/local' || true)"
if [ -n "${LEAK}" ]; then
    echo "ERROR: vendored SDL3 links non-system libraries:"
    echo "${LEAK}" | sed 's/^/    /'
    exit 1
fi
echo "  clean -- system frameworks only"

# The version the header claims and the version the built library reports at
# runtime are different things; a stale build tree could silently produce the
# latter. Assert the real one, and assert the GPU entry points photoscoping's
# whole renderer depends on are actually present in this build.
echo
echo "verifying runtime version and GPU API..."
cat > "${BUILD_DIR}/sdlchk.c" <<'EOF'
#include <stdio.h>
#include <dlfcn.h>

int main(int argc, char **argv) {
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) { printf("  FAIL: %s\n", dlerror()); return 1; }

    int (*get_version)(void) = (int (*)(void))dlsym(h, "SDL_GetVersion");
    if (!get_version) { printf("  FAIL: SDL_GetVersion not found\n"); return 1; }
    int v = get_version();
    int major = v / 1000000, minor = (v / 1000) % 1000, patch = v % 1000;
    printf("  runtime version: %d.%d.%d\n", major, minor, patch);
    if (v != %%EXPECTED%%) {
        printf("  FAIL: expected %%EXPECTED%% (%%VERSION%%), got %d\n", v);
        return 1;
    }

    /* The renderer is built entirely on these. If SDL were ever configured
       without GPU support the failure would otherwise surface as a confusing
       link error in photoscoping rather than here. */
    const char *gpu[] = {
        "SDL_CreateGPUDevice",
        "SDL_ClaimWindowForGPUDevice",
        "SDL_SetGPUSwapchainParameters",
        "SDL_WindowSupportsGPUSwapchainComposition",
        "SDL_GetGPUSwapchainTextureFormat",
        "SDL_AcquireGPUCommandBuffer",
        "SDL_WaitAndAcquireGPUSwapchainTexture",
        NULL
    };
    for (int i = 0; gpu[i]; i++) {
        if (!dlsym(h, gpu[i])) { printf("  FAIL: %s not found\n", gpu[i]); return 1; }
    }
    printf("  gpu api symbols resolved: OK\n");
    return 0;
}
EOF
# SDL_VERSIONNUM(major, minor, patch) == major*1000000 + minor*1000 + patch
EXPECTED=$(( $(echo "${SDL_VERSION}" | cut -d. -f1) * 1000000 \
           + $(echo "${SDL_VERSION}" | cut -d. -f2) * 1000 \
           + $(echo "${SDL_VERSION}" | cut -d. -f3) ))
sed -i '' "s/%%EXPECTED%%/${EXPECTED}/g; s/%%VERSION%%/${SDL_VERSION}/g" "${BUILD_DIR}/sdlchk.c"

clang "${BUILD_DIR}/sdlchk.c" -o "${BUILD_DIR}/sdlchk"
if ! "${BUILD_DIR}/sdlchk" "${SDL_LIB}"; then
    echo "ERROR: vendored SDL3 failed verification. Refusing to continue."
    exit 1
fi

echo "${STAMP_VALUE}" > "${STAMP}"
echo
echo "vendored SDL3 ${SDL_VERSION} built at ${PREFIX}"
du -sh "${PREFIX}/lib"

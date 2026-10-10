#!/bin/bash
# build_ffmpeg.sh - Build a minimal, LGPL, decode-only ffmpeg for photoscoping.
#
# Why vendor instead of using Homebrew's ffmpeg:
#   * License. Homebrew builds with --enable-gpl --enable-version3 (it wants
#     x264/x265), so its libraries report "GPL version 3 or later". Bundling
#     those into photoscoping would relicense the whole app as GPLv3, which
#     conflicts with photoscoping's LGPL-2.1. This build is LGPL-2.1.
#   * Size. Homebrew's ffmpeg tree costs ~33MB in the app bundle, most of it
#     encoders (x265 alone is 7MB) that photoscoping never calls. It only
#     ever decodes.
#
# Audio decoders are enabled as well as video: scrubbing footage without its
# sync sound is half the job. Camera originals carry PCM far more often than
# AAC, and QuickTime files from cameras store it big-endian, so both byte
# orders are in the list.
#
# Output: vendor/ffmpeg/{include,lib}. CMakeLists.txt picks this up
# automatically via PKG_CONFIG_PATH.

set -euo pipefail

FFMPEG_VERSION="8.1.2"
FFMPEG_SHA256="464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${SCRIPT_DIR}/ffmpeg"
CACHE_DIR="${SCRIPT_DIR}/.cache"
BUILD_DIR="${SCRIPT_DIR}/.build"
TARBALL="${CACHE_DIR}/ffmpeg-${FFMPEG_VERSION}.tar.xz"
SRC_DIR="${BUILD_DIR}/ffmpeg-${FFMPEG_VERSION}"

if [ "${1:-}" = "--clean" ]; then
    echo "removing ${PREFIX} and ${BUILD_DIR}..."
    rm -rf "${PREFIX}" "${BUILD_DIR}"
    exit 0
fi

# Already built? The stamp records the version so a version bump rebuilds.
# Keyed on the script's own hash as well as the version, so editing the
# configure flags below forces a rebuild instead of silently reusing the old one.
STAMP="${PREFIX}/.photoscoping-ffmpeg-stamp"
STAMP_VALUE="${FFMPEG_VERSION}-$(shasum -a 256 "$0" | cut -c1-12)"
if [ -f "${STAMP}" ] && [ "$(cat "${STAMP}")" = "${STAMP_VALUE}" ]; then
    echo "vendored ffmpeg ${FFMPEG_VERSION} already built at ${PREFIX}"
    echo "(run '$0 --clean' to force a rebuild)"
    exit 0
fi

mkdir -p "${CACHE_DIR}" "${BUILD_DIR}"

if [ ! -f "${TARBALL}" ]; then
    echo "downloading ffmpeg ${FFMPEG_VERSION}..."
    curl -fL --retry 3 -o "${TARBALL}.tmp" \
        "https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz"
    mv "${TARBALL}.tmp" "${TARBALL}"
fi

echo "verifying checksum..."
ACTUAL="$(shasum -a 256 "${TARBALL}" | awk '{print $1}')"
if [ "${ACTUAL}" != "${FFMPEG_SHA256}" ]; then
    echo "ERROR: checksum mismatch for ${TARBALL}"
    echo "  expected ${FFMPEG_SHA256}"
    echo "  actual   ${ACTUAL}"
    exit 1
fi

echo "extracting..."
rm -rf "${SRC_DIR}"
tar -xf "${TARBALL}" -C "${BUILD_DIR}"

cd "${SRC_DIR}"

# --disable-gpl AND --disable-version3: with version3 alone the result is
# LGPLv3, which still isn't compatible with photoscoping's LGPL-2.1. Both off gives
# LGPL-2.1, verified by the avcodec_license() check at the end of this script.
#
# No --enable-lib* at all. Every external codec library Homebrew links (x264,
# x265, svt-av1, dav1d, vpx, opus, lame, openssl) is dropped: they're all
# encoders, alternative decoders for codecs ffmpeg decodes natively, or TLS.
# That takes the ffmpeg dylibs in the bundle from 14 to 5.
#
# --disable-everything plus an explicit decode-only allowlist. Enabling every
# built-in decoder instead costs ~7.7MB more for codecs no camera or edit
# suite produces (VC-1/WMV, RealVideo, Cinepak, Indeo). The list below covers
# delivery formats plus the intermediates live action footage arrives in:
# ProRes, DNxHD/DNxHR (usually inside MXF) and CineForm. To widen it, add to
# --enable-decoder/parser/demuxer; the stamp hash above forces a rebuild.
#
# xlib/libxcb are [autodetect] and WILL turn themselves on if X11 headers are
# present on the build machine (Homebrew installs them as a transitive dep of
# plenty of things), silently adding libX11/libxcb/libXau/libXdmcp -- ~1.3MB of
# X11 -- to a macOS-only app bundle. Forced off.
#
# Note the bitstream filters: h264/hevc_mp4toannexb and extract_extradata are
# what let the VideoToolbox hwaccel path work on MP4-contained H.264/HEVC.
# Without them --disable-everything produces a build that opens files but
# decodes nothing.
#
# --enable-videotoolbox alone is not enough under --disable-everything. It turns
# on the VideoToolbox support code; the per-codec hwaccel entries are separate
# and get disabled with everything else. Without them avcodec_get_hw_config()
# returns nothing, FFmpeg quietly negotiates a software pixel format, and the
# decoder runs on the CPU while looking for all the world like it is
# accelerated. Hence the explicit --enable-hwaccel list.
echo "configuring (LGPL, decode-only)..."
./configure \
    --prefix="${PREFIX}" \
    --disable-gpl \
    --disable-version3 \
    --disable-nonfree \
    --enable-shared \
    --disable-static \
    --disable-programs \
    --disable-doc \
    --disable-avdevice \
    --disable-avfilter \
    --disable-devices \
    --disable-network \
    --disable-debug \
    --disable-xlib \
    --disable-libxcb \
    --disable-libxcb-shm \
    --disable-libxcb-xfixes \
    --disable-libxcb-shape \
    --enable-videotoolbox \
    --enable-pthreads \
    --disable-everything \
    --enable-swresample \
    --enable-decoder=h264,hevc,prores,dnxhd,cfhd,mpeg4,mpeg2video,mpeg1video,mjpeg,vp8,vp9,av1,theora,rawvideo,qtrle,png \
    --enable-decoder=aac,aac_latm,mp3,ac3,eac3,alac,flac,vorbis,opus,pcm_s16le,pcm_s24le,pcm_s32le,pcm_f32le,pcm_s16be,pcm_s24be,pcm_s32be,pcm_u8 \
    --enable-parser=h264,hevc,dnxhd,mpeg4video,mpegvideo,mjpeg,vp8,vp9,av1,png \
    --enable-parser=aac,aac_latm,mpegaudio,ac3,flac,vorbis,opus \
    --enable-demuxer=mov,mxf,matroska,avi,mpegts,mpegps,flv,image2 \
    --enable-demuxer=wav,mp3,ogg,flac,aac,ac3,w64,aiff,caf \
    --enable-hwaccel=h264_videotoolbox,hevc_videotoolbox,prores_videotoolbox,vp9_videotoolbox,av1_videotoolbox,mpeg4_videotoolbox \
    --enable-protocol=file \
    --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,extract_extradata,vp9_superframe,av1_frame_split,prores_metadata

echo "compiling (this takes a few minutes)..."
make -j"$(sysctl -n hw.ncpu)"

echo "installing to ${PREFIX}..."
rm -rf "${PREFIX}"
make install

# Ship the license text next to the libraries; LGPL redistribution requires
# conveying it, and releases/package.sh copies it into the app bundle.
cp COPYING.LGPLv2.1 "${PREFIX}/COPYING.LGPLv2.1"
cat > "${PREFIX}/README.photoscoping" <<EOF
FFmpeg ${FFMPEG_VERSION}, built for photoscoping under LGPL-2.1.

Unmodified upstream source: https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz
sha256: ${FFMPEG_SHA256}

Built by vendor/build_ffmpeg.sh; see that script for the exact configure line.
These libraries are dynamically linked, so they can be replaced with a
modified build by substituting the dylibs in photoscoping.app/Contents/Frameworks/.
EOF

echo "${STAMP_VALUE}" > "${STAMP}"

echo
# The configure line is long and a typo in it fails silently -- the library
# builds fine and simply cannot decode the format. Assert the codecs that
# matter, rather than trusting the flags.
echo
echo "verifying required decoders are present..."
cat > "${BUILD_DIR}/codecchk.c" <<'EOF'
#include <stdio.h>
#include <libavcodec/avcodec.h>
int main(void) {
    /* The camera and intermediate codecs first: those are what photoscoping
       exists to read. h264/hevc and aac cover delivery files. */
    const enum AVCodecID want[] = {
        AV_CODEC_ID_PRORES, AV_CODEC_ID_DNXHD, AV_CODEC_ID_CFHD,
        AV_CODEC_ID_PCM_S24LE, AV_CODEC_ID_PCM_S24BE,
        AV_CODEC_ID_H264, AV_CODEC_ID_HEVC,
        AV_CODEC_ID_AAC, AV_CODEC_ID_MP3, AV_CODEC_ID_ALAC,
        AV_CODEC_ID_FLAC, AV_CODEC_ID_PCM_S16LE, AV_CODEC_ID_VORBIS,
        AV_CODEC_ID_OPUS,
    };
    int bad = 0;
    for (unsigned i = 0; i < sizeof(want)/sizeof(*want); ++i) {
        const AVCodec *c = avcodec_find_decoder(want[i]);
        if (!c) { printf("  MISSING decoder id %d\n", (int)want[i]); bad = 1; }
    }
    if (!bad) printf("  all required video and audio decoders present\n");
    return bad;
}
EOF
cc -I"${PREFIX}/include" "${BUILD_DIR}/codecchk.c" -o "${BUILD_DIR}/codecchk" \
   -L"${PREFIX}/lib" -lavcodec -lavutil -Wl,-rpath,"${PREFIX}/lib"
if ! "${BUILD_DIR}/codecchk"; then
    echo "ERROR: vendored ffmpeg is missing required decoders."
    exit 1
fi

echo
echo "verifying videotoolbox hwaccels are present..."
cat > "${BUILD_DIR}/hwchk.c" <<'EOF'
#include <stdio.h>
#include <libavcodec/avcodec.h>
/* A decoder can exist while its hwaccel does not, and the difference is
   invisible until you notice the CPU is doing the work. Assert the hw configs
   rather than the configure flag. */
static int has_vt(enum AVCodecID id, const char *name) {
    const AVCodec *c = avcodec_find_decoder(id);
    if (!c) { printf("  MISSING decoder %s\n", name); return 1; }
    for (int i = 0;; ++i) {
        const AVCodecHWConfig *cfg = avcodec_get_hw_config(c, i);
        if (!cfg) break;
        if (cfg->device_type == AV_HWDEVICE_TYPE_VIDEOTOOLBOX) return 0;
    }
    printf("  MISSING videotoolbox hwaccel for %s\n", name);
    return 1;
}
int main(void) {
    int bad = has_vt(AV_CODEC_ID_H264, "h264") | has_vt(AV_CODEC_ID_HEVC, "hevc");
    if (!bad) printf("  h264 and hevc both offer videotoolbox\n");
    return bad;
}
EOF
cc -I"${PREFIX}/include" "${BUILD_DIR}/hwchk.c" -o "${BUILD_DIR}/hwchk" \
   -L"${PREFIX}/lib" -lavcodec -lavutil -Wl,-rpath,"${PREFIX}/lib"
if ! "${BUILD_DIR}/hwchk"; then
    echo "ERROR: vendored ffmpeg has no videotoolbox hwaccels."
    exit 1
fi

echo "verifying the build is actually LGPL..."
cat > "${BUILD_DIR}/licchk.c" <<'EOF'
#include <stdio.h>
#include <string.h>
const char *avcodec_license(void);
const char *avformat_license(void);
const char *avutil_license(void);
const char *swscale_license(void);
int main(void) {
    const char *l[4] = { avcodec_license(), avformat_license(),
                         avutil_license(), swscale_license() };
    const char *n[4] = { "libavcodec", "libavformat", "libavutil", "libswscale" };
    int bad = 0;
    for (int i = 0; i < 4; i++) {
        printf("  %-12s %s\n", n[i], l[i]);
        if (strncmp(l[i], "LGPL version 2.1", 16) != 0) bad = 1;
    }
    return bad;
}
EOF
clang "${BUILD_DIR}/licchk.c" -o "${BUILD_DIR}/licchk" \
    -I"${PREFIX}/include" \
    "${PREFIX}/lib/libavcodec.dylib" "${PREFIX}/lib/libavformat.dylib" \
    "${PREFIX}/lib/libavutil.dylib" "${PREFIX}/lib/libswscale.dylib"
if ! "${BUILD_DIR}/licchk"; then
    echo "ERROR: vendored ffmpeg is not LGPL-2.1. Refusing to continue."
    exit 1
fi

echo
echo "verifying no X11 dependency leaked in..."
X11LEAK="$(otool -L "${PREFIX}/lib/libavcodec.62.dylib" "${PREFIX}/lib/libavutil.60.dylib" \
    | grep -oE "lib(X11|xcb|Xau|Xdmcp)\.[0-9.]*dylib" | sort -u || true)"
if [ -n "${X11LEAK}" ]; then
    echo "ERROR: vendored ffmpeg links X11:"
    echo "${X11LEAK}" | sed 's/^/    /'
    exit 1
fi
echo "  clean -- no X11"

echo
echo "vendored ffmpeg ${FFMPEG_VERSION} built at ${PREFIX}"
du -sh "${PREFIX}/lib"

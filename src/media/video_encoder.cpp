#include "media/video_encoder.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <thread>

namespace ps::media {

namespace {

std::string av_error(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof text);
    return text;
}

struct Recipe {
    const char* encoder;
    const char* muxer;
    AVPixelFormat pixels;
};

Recipe recipe_for(ExportFormat format) {
    switch (format) {
        case ExportFormat::kProRes4444: return {"prores_ks", "mov", AV_PIX_FMT_YUVA444P10LE};
        case ExportFormat::kVp9Alpha: return {"libvpx-vp9", "webm", AV_PIX_FMT_YUVA420P};
        case ExportFormat::kMatteProRes422:
        case ExportFormat::kMatteProRes422Lt: return {"prores_ks", "mov", AV_PIX_FMT_YUV422P10LE};
        case ExportFormat::kMatteHevc: return {"hevc_videotoolbox", "mp4", AV_PIX_FMT_P010LE};
        case ExportFormat::kMatteH264: return {"h264_videotoolbox", "mp4", AV_PIX_FMT_NV12};
        case ExportFormat::kPngSequence: break;
    }
    return {nullptr, nullptr, AV_PIX_FMT_NONE};
}

}  // namespace

VideoEncoder::~VideoEncoder() { close(); }

bool VideoEncoder::open(const std::string& path, ExportFormat format, int width, int height,
                        int rate_num, int rate_den, std::string* error) {
    close();
    const Recipe r = recipe_for(format);
    if (!r.encoder) {
        *error = "not a video format";
        return false;
    }
    format_ = format;
    width_ = width;
    height_ = height;
    const bool subject = export_format_info(format).subject;

    const AVCodec* codec = avcodec_find_encoder_by_name(r.encoder);
    if (!codec) {
        *error = std::string("this build has no ") + r.encoder + " encoder";
        return false;
    }
    int ret = avformat_alloc_output_context2(&format_ctx_, nullptr, r.muxer, path.c_str());
    if (ret < 0 || !format_ctx_) {
        *error = "cannot make the file: " + av_error(ret);
        return false;
    }
    stream_ = avformat_new_stream(format_ctx_, nullptr);
    codec_ = avcodec_alloc_context3(codec);
    if (!stream_ || !codec_) {
        *error = "out of memory";
        return false;
    }

    if (rate_num <= 0 || rate_den <= 0) {
        rate_num = 24;
        rate_den = 1;
    }
    codec_->width = width;
    codec_->height = height;
    codec_->pix_fmt = r.pixels;
    codec_->time_base = AVRational{rate_den, rate_num};
    codec_->framerate = AVRational{rate_num, rate_den};
    codec_->sample_aspect_ratio = AVRational{1, 1};
    codec_->color_primaries = AVCOL_PRI_BT709;
    codec_->color_trc = AVCOL_TRC_BT709;
    codec_->colorspace = AVCOL_SPC_BT709;
    codec_->color_range = AVCOL_RANGE_MPEG;
    codec_->thread_count = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    if (format_ctx_->oformat->flags & AVFMT_GLOBALHEADER)
        codec_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    void* priv = codec_->priv_data;
    switch (format) {
        case ExportFormat::kProRes4444:
            av_opt_set(priv, "profile", "4444", 0);
            av_opt_set_int(priv, "alpha_bits", 16, 0);
            av_opt_set(priv, "vendor", "apl0", 0);
            break;
        case ExportFormat::kMatteProRes422:
            av_opt_set(priv, "profile", "standard", 0);
            av_opt_set(priv, "vendor", "apl0", 0);
            break;
        case ExportFormat::kMatteProRes422Lt:
            av_opt_set(priv, "profile", "lt", 0);
            av_opt_set(priv, "vendor", "apl0", 0);
            break;
        case ExportFormat::kVp9Alpha:
            // Constant quality, near enough to visually lossless for a
            // subject on transparency, at a speed that is not overnight.
            codec_->bit_rate = 0;
            av_opt_set_int(priv, "crf", 24, 0);
            av_opt_set(priv, "deadline", "good", 0);
            av_opt_set_int(priv, "cpu-used", 2, 0);
            av_opt_set_int(priv, "row-mt", 1, 0);
            break;
        case ExportFormat::kMatteHevc:
        case ExportFormat::kMatteH264:
            // VideoToolbox's constant quality, 0-100, high: a matte is mostly
            // flat, and what is not is the edge it exists for.
            codec_->flags |= AV_CODEC_FLAG_QSCALE;
            codec_->global_quality = FF_QP2LAMBDA * 80;
            av_opt_set(priv, "profile", format == ExportFormat::kMatteHevc ? "main10" : "high", 0);
            // hvc1 rather than hev1: QuickTime and Final Cut will not play
            // the other.
            if (format == ExportFormat::kMatteHevc) codec_->codec_tag = MKTAG('h', 'v', 'c', '1');
            break;
        case ExportFormat::kPngSequence: break;
    }

    ret = avcodec_open2(codec_, codec, nullptr);
    if (ret < 0) {
        *error = std::string("cannot start the ") + r.encoder + " encoder: " + av_error(ret);
        return false;
    }
    avcodec_parameters_from_context(stream_->codecpar, codec_);
    stream_->time_base = codec_->time_base;
    stream_->avg_frame_rate = codec_->framerate;

    ret = avio_open(&format_ctx_->pb, path.c_str(), AVIO_FLAG_WRITE);
    if (ret < 0) {
        *error = "cannot write " + path + ": " + av_error(ret);
        return false;
    }
    ret = avformat_write_header(format_ctx_, nullptr);
    if (ret < 0) {
        *error = "cannot write the header: " + av_error(ret);
        return false;
    }
    header_written_ = true;

    frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    frame_->format = r.pixels;
    frame_->width = width;
    frame_->height = height;
    if (!frame_ || !packet_ || av_frame_get_buffer(frame_, 0) < 0) {
        *error = "out of memory";
        return false;
    }

    // RGBA or grey, full range, to the encoder's Y'CbCr in video range by
    // the Rec.709 matrix. Grey becomes luma as it is, chroma neutral.
    const AVPixelFormat source = subject ? AV_PIX_FMT_RGBA64LE : AV_PIX_FMT_GRAY16LE;
    sws_ = sws_getContext(width, height, source, width, height, r.pixels,
                          SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT, nullptr, nullptr,
                          nullptr);
    if (!sws_) {
        *error = "cannot convert to the encoder's pixels";
        return false;
    }
    const int* bt709 = sws_getCoefficients(SWS_CS_ITU709);
    sws_setColorspaceDetails(sws_, bt709, /*src full range*/ 1, bt709, /*dst video range*/ 0, 0,
                             1 << 16, 1 << 16);
    next_pts_ = 0;
    return true;
}

bool VideoEncoder::write(const std::uint16_t* samples, std::string* error) {
    if (!codec_) return false;
    if (av_frame_make_writable(frame_) < 0) {
        *error = "out of memory";
        return false;
    }
    const bool subject = export_format_info(format_).subject;
    const std::uint8_t* src[1] = {reinterpret_cast<const std::uint8_t*>(samples)};
    const int src_stride[1] = {width_ * (subject ? 8 : 2)};
    sws_scale(sws_, src, src_stride, 0, height_, frame_->data, frame_->linesize);
    frame_->pts = next_pts_++;
    frame_->color_primaries = codec_->color_primaries;
    frame_->color_trc = codec_->color_trc;
    frame_->colorspace = codec_->colorspace;
    frame_->color_range = codec_->color_range;
    const int ret = avcodec_send_frame(codec_, frame_);
    if (ret < 0) {
        *error = "the encoder refused a frame: " + av_error(ret);
        return false;
    }
    return drain(false, error);
}

bool VideoEncoder::drain(bool flushing, std::string* error) {
    while (true) {
        const int ret = avcodec_receive_packet(codec_, packet_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return true;
        if (ret < 0) {
            *error = "the encoder failed: " + av_error(ret);
            return false;
        }
        av_packet_rescale_ts(packet_, codec_->time_base, stream_->time_base);
        packet_->stream_index = stream_->index;
        const int wrote = av_interleaved_write_frame(format_ctx_, packet_);
        if (wrote < 0) {
            *error = "cannot write a frame: " + av_error(wrote);
            return false;
        }
    }
    (void)flushing;
}

bool VideoEncoder::finish(std::string* error) {
    if (!codec_) return false;
    bool ok = avcodec_send_frame(codec_, nullptr) >= 0 && drain(true, error);
    if (header_written_) {
        const int ret = av_write_trailer(format_ctx_);
        if (ret < 0 && ok) {
            *error = "cannot finish the file: " + av_error(ret);
            ok = false;
        }
        header_written_ = false;
    }
    close();
    return ok;
}

void VideoEncoder::close() {
    if (sws_) sws_freeContext(sws_);
    sws_ = nullptr;
    av_frame_free(&frame_);
    av_packet_free(&packet_);
    avcodec_free_context(&codec_);
    if (format_ctx_) {
        if (format_ctx_->pb) avio_closep(&format_ctx_->pb);
        avformat_free_context(format_ctx_);
    }
    format_ctx_ = nullptr;
    stream_ = nullptr;
}

}  // namespace ps::media

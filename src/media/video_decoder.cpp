#include "media/video_decoder.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

#include <SDL3/SDL.h>

#include <cmath>

namespace ps::media {

namespace {

// Without a get_format that picks AV_PIX_FMT_VIDEOTOOLBOX, FFmpeg quietly
// negotiates a software format and decodes on the CPU while every other
// signal says hardware is in use (found the hard way in understory).
AVPixelFormat pick_hw_format(AVCodecContext*, const AVPixelFormat* formats) {
    for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p)
        if (*p == AV_PIX_FMT_VIDEOTOOLBOX) return *p;
    return formats[0];
}

}  // namespace

VideoDecoder::~VideoDecoder() { close(); }

void VideoDecoder::close() {
    av_frame_free(&frame_);
    av_frame_free(&decoded_);
    av_packet_free(&packet_);
    avcodec_free_context(&codec_);
    av_buffer_unref(&hw_device_);
    avformat_close_input(&format_);
    stream_ = -1;
    frame_index_ = -1;
    described_format_ = -1;
    draining_ = false;
}

bool VideoDecoder::open(const std::string& path, bool hardware) {
    close();
    if (const int err = avformat_open_input(&format_, path.c_str(), nullptr, nullptr); err < 0) {
        char reason[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(err, reason, sizeof reason);
        SDL_Log("decoder: cannot open %s: %s", path.c_str(), reason);
        return false;
    }
    if (avformat_find_stream_info(format_, nullptr) < 0) {
        SDL_Log("decoder: no stream info in %s", path.c_str());
        close();
        return false;
    }
    const AVCodec* decoder = nullptr;
    stream_ = av_find_best_stream(format_, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (stream_ < 0 || !decoder) {
        SDL_Log("decoder: no decodable video stream in %s", path.c_str());
        close();
        return false;
    }
    AVStream* stream = format_->streams[stream_];
    codec_ = avcodec_alloc_context3(decoder);
    if (!codec_ || avcodec_parameters_to_context(codec_, stream->codecpar) < 0) {
        close();
        return false;
    }
    codec_name_ = decoder->name;

    if (hardware) {
        for (int i = 0;; ++i) {
            const AVCodecHWConfig* cfg = avcodec_get_hw_config(decoder, i);
            if (!cfg) break;
            if (cfg->device_type == AV_HWDEVICE_TYPE_VIDEOTOOLBOX &&
                (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
                if (av_hwdevice_ctx_create(&hw_device_, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr,
                                           nullptr, 0) >= 0) {
                    codec_->hw_device_ctx = av_buffer_ref(hw_device_);
                    codec_->get_format = pick_hw_format;
                }
                break;
            }
        }
    }
    // Software decode uses every core; ProRes and HEVC both slice-thread.
    codec_->thread_count = 0;

    if (avcodec_open2(codec_, decoder, nullptr) < 0) {
        SDL_Log("decoder: cannot open the %s decoder", decoder->name);
        close();
        return false;
    }

    width_ = codec_->width;
    height_ = codec_->height;
    const AVRational rate = stream->avg_frame_rate.num ? stream->avg_frame_rate : stream->r_frame_rate;
    frame_rate_ = rate.num ? av_q2d(rate) : 0.0;
    start_pts_ = stream->start_time != AV_NOPTS_VALUE ? stream->start_time : 0;
    if (stream->nb_frames > 0) {
        frame_count_ = stream->nb_frames;
    } else if (stream->duration > 0 && frame_rate_ > 0) {
        frame_count_ = std::llround(stream->duration * av_q2d(stream->time_base) * frame_rate_);
    }

    packet_ = av_packet_alloc();
    decoded_ = av_frame_alloc();
    frame_ = av_frame_alloc();
    return packet_ && decoded_ && frame_;
}

std::int64_t VideoDecoder::index_of(std::int64_t pts) const {
    const AVStream* stream = format_->streams[stream_];
    return std::llround((pts - start_pts_) * av_q2d(stream->time_base) * frame_rate_);
}

bool VideoDecoder::receive_frame() {
    for (;;) {
        const int got = avcodec_receive_frame(codec_, decoded_);
        if (got == 0) break;
        if (got == AVERROR_EOF) return false;
        if (got != AVERROR(EAGAIN)) {
            SDL_Log("decoder: decode error %d", got);
            return false;
        }
        if (draining_) return false;
        // Feed packets until the decoder has a frame for us.
        for (;;) {
            if (av_read_frame(format_, packet_) < 0) {
                draining_ = true;
                avcodec_send_packet(codec_, nullptr);
                break;
            }
            if (packet_->stream_index == stream_) {
                const int sent = avcodec_send_packet(codec_, packet_);
                av_packet_unref(packet_);
                if (sent < 0 && sent != AVERROR(EAGAIN)) {
                    SDL_Log("decoder: corrupt packet (%d), skipping", sent);
                    continue;
                }
                break;
            }
            av_packet_unref(packet_);
        }
    }

    av_frame_unref(frame_);
    if (decoded_->format == AV_PIX_FMT_VIDEOTOOLBOX) {
        // Back to system memory, in the transfer format VideoToolbox picked for
        // this source's depth and subsampling (p010, p210, p410, nv12...).
        if (av_hwframe_transfer_data(frame_, decoded_, 0) < 0) {
            SDL_Log("decoder: could not transfer the frame from VideoToolbox");
            return false;
        }
        av_frame_copy_props(frame_, decoded_);
    } else {
        av_frame_move_ref(frame_, decoded_);
    }
    av_frame_unref(decoded_);

    const std::int64_t pts = frame_->best_effort_timestamp != AV_NOPTS_VALUE
                                 ? frame_->best_effort_timestamp
                                 : frame_->pts;
    frame_index_ = pts != AV_NOPTS_VALUE ? index_of(pts) : frame_index_ + 1;
    return describe_frame(frame_);
}

bool VideoDecoder::describe_frame(const AVFrame* frame) {
    // Colour tags can only be read from a frame; the codec context's copies
    // are not filled in for every container.
    signal_.range_tagged = frame->color_range != AVCOL_RANGE_UNSPECIFIED;
    signal_.range = frame->color_range == AVCOL_RANGE_JPEG ? color::SignalRange::kFull
                                                           : color::SignalRange::kLimited;
    signal_.matrix_tagged = true;
    switch (frame->colorspace) {
        case AVCOL_SPC_BT709: signal_.matrix = color::YcbcrMatrix::kBt709; break;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL: signal_.matrix = color::YcbcrMatrix::kBt2020; break;
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M: signal_.matrix = color::YcbcrMatrix::kBt601; break;
        default:
            // Untagged: HD and up is 709 by convention.
            signal_.matrix_tagged = false;
            signal_.matrix = color::YcbcrMatrix::kBt709;
            break;
    }
    const char* primaries = av_color_primaries_name(frame->color_primaries);
    const char* transfer = av_color_transfer_name(frame->color_trc);
    signal_.primaries = primaries ? primaries : "unknown";
    signal_.transfer = transfer ? transfer : "unknown";

    if (frame->format == described_format_) return true;

    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
    const int planes = av_pix_fmt_count_planes(static_cast<AVPixelFormat>(frame->format));
    if (!d || (d->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL)) ||
        d->nb_components < 3 || (planes != 2 && planes != 3 && planes != 4) ||
        d->comp[0].depth > 16) {
        SDL_Log("decoder: unsupported pixel format %s", d ? d->name : "?");
        return false;
    }
    FrameLayout layout;
    layout.width = frame->width;
    layout.height = frame->height;
    layout.chroma_width = AV_CEIL_RSHIFT(frame->width, d->log2_chroma_w);
    layout.chroma_height = AV_CEIL_RSHIFT(frame->height, d->log2_chroma_h);
    // A fourth plane is ProRes 4444's alpha; it plays no part in the colour.
    layout.interleaved_chroma = planes == 2;
    layout.bit_depth = d->comp[0].depth;
    layout.bytes_per_sample = (d->comp[0].depth + d->comp[0].shift) > 8 ? 2 : 1;
    const float max_code = static_cast<float>((1 << layout.bit_depth) - 1);
    const float stored_max = layout.bytes_per_sample == 2 ? 65535.0f : 255.0f;
    layout.sample_scale = stored_max / (max_code * static_cast<float>(1 << d->comp[0].shift));
    layout_ = layout;
    described_format_ = frame->format;

    SDL_Log("decoder: %dx%d %s via %s, %d-bit, chroma %dx%d%s", layout.width, layout.height,
            d->name, hardware_decoding() ? "VideoToolbox" : "software", layout.bit_depth,
            layout.chroma_width, layout.chroma_height,
            layout.interleaved_chroma ? " interleaved" : "");
    return true;
}

bool VideoDecoder::read_next() { return format_ && receive_frame(); }

bool VideoDecoder::seek_frame(std::int64_t index) {
    if (!format_ || index < 0 || (frame_count_ > 0 && index >= frame_count_)) return false;
    if (index == frame_index_) return true;
    // Stepping forward a little is cheaper than a seek, which restarts from a
    // keyframe; anything else seeks.
    if (index < frame_index_ || index > frame_index_ + 30 || frame_index_ < 0) {
        const AVStream* stream = format_->streams[stream_];
        const std::int64_t target =
            start_pts_ + std::llround(index / frame_rate_ / av_q2d(stream->time_base));
        if (av_seek_frame(format_, stream_, target, AVSEEK_FLAG_BACKWARD) < 0) {
            SDL_Log("decoder: seek to frame %lld failed", static_cast<long long>(index));
            return false;
        }
        avcodec_flush_buffers(codec_);
        draining_ = false;
        frame_index_ = -1;
    }
    while (frame_index_ < index) {
        if (!receive_frame()) return false;
    }
    return frame_index_ == index;
}

const std::uint8_t* VideoDecoder::plane(int i) const { return frame_ ? frame_->data[i] : nullptr; }

int VideoDecoder::stride(int i) const { return frame_ ? frame_->linesize[i] : 0; }

}  // namespace ps::media

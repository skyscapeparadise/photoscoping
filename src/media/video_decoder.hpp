#pragma once

#include <cstdint>
#include <string>

#include "color/ycbcr.hpp"

struct AVBufferRef;
struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;

namespace ps::media {

// The planes of one decoded frame, described generically from FFmpeg's pixel
// format descriptor rather than by a list of known formats. Camera files
// arrive in more layouts than playback ever sees -- ProRes 4:2:2 and 4:4:4,
// XF-HEVC 4:2:2 10-bit, VideoToolbox's p010/p210/p410 transfer formats -- and
// all of them reduce to: a luma plane, then either one interleaved chroma
// plane or two separate ones, at some subsampling and bit depth.
struct FrameLayout {
    int width = 0, height = 0;
    int chroma_width = 0, chroma_height = 0;
    bool interleaved_chroma = false;  // Cb/Cr in one two-channel plane
    int bytes_per_sample = 1;         // 1 or 2
    int bit_depth = 8;
    // Multiplier that turns a UNORM texture sample into code / (2^depth - 1).
    // 10-bit planar formats keep their value in the low bits of a 16-bit word
    // (a sample reads value/65535); p010 keeps it in the high bits.
    float sample_scale = 1.0f;
};

// Colour tags read from the file. Each is what the file says, or a stated
// guess when the file is silent -- never silently assumed.
struct SignalInfo {
    color::YcbcrMatrix matrix = color::YcbcrMatrix::kBt709;
    color::SignalRange range = color::SignalRange::kLimited;
    bool matrix_tagged = false;
    bool range_tagged = false;
    std::string primaries;  // as FFmpeg names them, for display
    std::string transfer;
};

// Frame-accurate decode of one video stream.
//
// Synchronous on purpose: photoscoping steps through footage one exact frame
// at a time (every frame gets matted), so the call that asks for frame N is
// the one that waits for it. A read-ahead thread is the playback
// optimisation to add when playback needs it.
class VideoDecoder {
public:
    VideoDecoder() = default;
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    bool open(const std::string& path, bool hardware = true);
    void close();

    // Decodes the next frame in presentation order. False at end of stream or
    // on an error, which is logged.
    bool read_next();
    // Positions on exactly frame `index` (0-based), decoding forward from the
    // preceding keyframe. False if the frame does not exist.
    bool seek_frame(std::int64_t index);

    // The current frame's planes, valid until the next read or seek. Plane 1
    // is interleaved CbCr when layout().interleaved_chroma; plane 2 is unused.
    const std::uint8_t* plane(int i) const;
    int stride(int i) const;
    const FrameLayout& layout() const { return layout_; }
    const SignalInfo& signal() const { return signal_; }

    std::int64_t frame_index() const { return frame_index_; }
    // Estimated from duration and rate; containers do not always say.
    std::int64_t frame_count() const { return frame_count_; }
    double frame_rate() const { return frame_rate_; }
    int width() const { return width_; }
    int height() const { return height_; }
    bool hardware_decoding() const { return hw_device_ != nullptr; }
    const std::string& codec_name() const { return codec_name_; }

private:
    bool receive_frame();
    bool describe_frame(const AVFrame* frame);
    std::int64_t index_of(std::int64_t pts) const;

    AVFormatContext* format_ = nullptr;
    AVCodecContext* codec_ = nullptr;
    AVBufferRef* hw_device_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVFrame* decoded_ = nullptr;   // as the decoder returns it (maybe on the GPU)
    AVFrame* frame_ = nullptr;     // in system memory, what plane() exposes
    int stream_ = -1;
    bool draining_ = false;

    int width_ = 0, height_ = 0;
    double frame_rate_ = 0.0;
    std::int64_t frame_count_ = 0;
    std::int64_t frame_index_ = -1;
    std::int64_t start_pts_ = 0;
    std::string codec_name_;
    FrameLayout layout_;
    SignalInfo signal_;
    int described_format_ = -1;
};

}  // namespace ps::media

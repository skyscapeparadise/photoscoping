#pragma once

#include <cstdint>
#include <string>

#include "media/export_format.hpp"

struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct AVStream;
struct SwsContext;

namespace ps::media {

// Writes an export, frame by frame, in one of the video ExportFormats.
//
// Frames go in as 16-bit samples and come out tagged Rec.709 throughout --
// primaries, transfer and matrix -- in the video range, which is what every
// editor reads these formats as:
//
//   the subject   RGBA, interleaved, straight (not premultiplied) alpha, the
//                 colour already encoded for Rec.709 -- ProRes 4444 at 10 bits
//                 4:4:4 with a 16-bit alpha; VP9 4:2:0 with its alpha in a
//                 second stream, as browsers expect
//   the matte     one plane of coverage, which becomes luma: white where the
//                 subject is. ProRes 422 and 422 LT at 10 bits, HEVC Main 10,
//                 H.264 High at 8, the last two on the Mac's own encoder
//
// The PNG sequence is not a video and is written elsewhere.
class VideoEncoder {
public:
    VideoEncoder() = default;
    ~VideoEncoder();
    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    // `rate` is the clip's, exactly: num/den frames a second.
    bool open(const std::string& path, ExportFormat format, int width, int height, int rate_num,
              int rate_den, std::string* error);
    // width x height x 4 samples for the subject's formats, width x height
    // for the matte's.
    bool write(const std::uint16_t* samples, std::string* error);
    // Flushes the encoder and closes the file. An export not finished is a
    // file not finished.
    bool finish(std::string* error);

private:
    bool drain(bool flushing, std::string* error);
    void close();

    ExportFormat format_ = ExportFormat::kProRes4444;
    int width_ = 0, height_ = 0;
    std::int64_t next_pts_ = 0;
    AVFormatContext* format_ctx_ = nullptr;
    AVCodecContext* codec_ = nullptr;
    AVStream* stream_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    SwsContext* sws_ = nullptr;
    bool header_written_ = false;
};

}  // namespace ps::media

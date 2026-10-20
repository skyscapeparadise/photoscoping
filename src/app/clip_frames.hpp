#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <memory>
#include <string>

#include "app/frame_source.hpp"
#include "color/cube_lut.hpp"
#include "gpu/ingest_pass.hpp"
#include "media/video_decoder.hpp"

namespace ps::app {

// The open clip, decoded on the worker and brought into the working space
// on the main thread.
class ClipFrames : public FrameSource {
public:
    ClipFrames() = default;
    ~ClipFrames() override { close(); }
    ClipFrames(const ClipFrames&) = delete;
    ClipFrames& operator=(const ClipFrames&) = delete;

    // `ingest` is the app's and outlives this. Opening does not decode: the
    // first frame comes when it is asked for, like every other.
    bool open(const std::string& path, bool hardware, gpu::IngestPass* ingest);
    void close();
    bool is_open() const { return decoder_ != nullptr; }

    // The LUT the signal goes through, or null for the file's own tags. The
    // shown frame is made again under it.
    void set_lut(const color::CubeLut* lut);

    // Fixed once the clip is open, so safe while the worker runs.
    const std::string& path() const { return path_; }
    double frame_rate() const { return frame_rate_; }
    std::int64_t frame_count() const { return frame_count_; }
    const media::VideoDecoder& decoder() const { return *decoder_; }

protected:
    bool upload(SDL_GPUCommandBuffer* cmd) override;

private:
    std::unique_ptr<media::VideoDecoder> decoder_;
    gpu::IngestPass* ingest_ = nullptr;
    const color::CubeLut* lut_ = nullptr;
    std::string path_;
    double frame_rate_ = 0.0;
    std::int64_t frame_count_ = 0;
};

}  // namespace ps::app

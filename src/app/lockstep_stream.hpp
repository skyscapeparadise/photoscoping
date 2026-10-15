#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>

#include "gpu/ingest_pass.hpp"
#include "media/video_decoder.hpp"

namespace ps::app {

// A render of the clip -- its matte, its subject -- that plays in step with
// it: frame N of the render is frame N of the clip, so it is asked for by the
// clip's frame index and never runs a clock of its own.
//
// Decoded only when asked, so a view that does not show it costs nothing, and
// switching to one that does catches it up in a single seek.
class LockstepStream {
public:
    // What the render's pixels are. A subject is a picture and decodes through
    // its own tags like any clip. A matte is coverage, not light: its code
    // values *are* the alpha, so they go into the working image untouched.
    enum class Kind { kPicture, kCoverage };

    LockstepStream() = default;
    LockstepStream(const LockstepStream&) = delete;
    LockstepStream& operator=(const LockstepStream&) = delete;

    bool open(SDL_GPUDevice* device, const std::string& path, Kind kind);
    void destroy();
    bool is_open() const { return open_; }

    // Brings the stream to `frame` and records the ingest into `cmd` if it
    // moved. False when the render has no such frame -- a matte made with
    // --limit stops short of the clip -- and then working() is not that frame.
    bool sync(SDL_GPUCommandBuffer* cmd, std::int64_t frame);

    SDL_GPUTexture* working() const { return ingest_.working(); }
    std::int64_t frame_count() const { return open_ ? decoder_.frame_count() : 0; }

private:
    media::VideoDecoder decoder_;
    gpu::IngestPass ingest_;
    Kind kind_ = Kind::kPicture;
    bool open_ = false;
    // The frame last ingested, and whether the last sync reached its frame.
    std::int64_t ingested_ = -1;
    bool in_step_ = false;
};

}  // namespace ps::app

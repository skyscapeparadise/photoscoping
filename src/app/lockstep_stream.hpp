#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>

#include "app/frame_source.hpp"
#include "gpu/ingest_pass.hpp"
#include "media/video_decoder.hpp"

namespace ps::app {

// A render of the clip -- its matte, its subject -- that plays in step with
// it: frame N of the render is frame N of the clip, so it is asked for by the
// clip's frame index and never runs a clock of its own.
//
// Decoded only when asked, so a view that does not show it costs nothing.
class LockstepStream : public FrameSource {
public:
    // What the render's pixels are. A subject is a picture and decodes through
    // its own tags like any clip. A matte is coverage, not light: its code
    // values *are* the alpha, so they go into the working image untouched.
    enum class Kind { kPicture, kCoverage };

    LockstepStream() = default;
    ~LockstepStream() override { destroy(); }
    LockstepStream(const LockstepStream&) = delete;
    LockstepStream& operator=(const LockstepStream&) = delete;

    bool open(SDL_GPUDevice* device, const std::string& path, Kind kind);
    void destroy();
    bool is_open() const { return open_; }

    SDL_GPUTexture* working() const { return ingest_.working(); }
    std::int64_t frame_count() const { return open_ ? decoder_.frame_count() : 0; }

protected:
    bool upload(SDL_GPUCommandBuffer* cmd) override;

private:
    media::VideoDecoder decoder_;
    gpu::IngestPass ingest_;
    Kind kind_ = Kind::kPicture;
    bool open_ = false;
};

}  // namespace ps::app

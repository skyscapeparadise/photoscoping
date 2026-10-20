#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "app/frame_source.hpp"

namespace ps::app {

// The mattes tools/matte_clip.py writes one frame at a time, as 16-bit PNGs
// named by frame index, read while the run that writes them is still going.
//
// Slower to show than the finished matte video -- a PNG is decoded on the CPU
// every time the frame changes, on the worker -- but there from the moment
// each frame is done, which the video is not until the whole clip is.
class MatteFrames : public FrameSource {
public:
    MatteFrames() = default;
    ~MatteFrames() override;
    MatteFrames(const MatteFrames&) = delete;
    MatteFrames& operator=(const MatteFrames&) = delete;

    void open(SDL_GPUDevice* device, const std::string& dir, std::int64_t frame_count);
    void destroy();

    // Looks at the directory again. True when anything changed.
    bool rescan();

    // Which frames have a matte, one entry per frame of the clip.
    const std::vector<bool>& done() const { return done_; }
    std::int64_t done_count() const { return done_count_; }
    bool matted(std::int64_t frame) const;
    // When the newest of them was written; the epoch when there are none.
    std::filesystem::file_time_type newest() const { return newest_; }

    // The shown frame's matte, coverage in red.
    SDL_GPUTexture* texture() const { return shown_ >= 0 ? texture_ : nullptr; }

protected:
    bool upload(SDL_GPUCommandBuffer* cmd) override;

private:
    // Decoded on the worker, uploaded from on the main thread.
    SDL_Surface* loaded_ = nullptr;

    SDL_GPUDevice* device_ = nullptr;
    std::string dir_;
    std::vector<bool> done_;
    std::int64_t done_count_ = 0;
    std::filesystem::file_time_type newest_{};

    SDL_GPUTexture* texture_ = nullptr;
    SDL_GPUTransferBuffer* transfer_ = nullptr;
    int width_ = 0, height_ = 0;
};

}  // namespace ps::app

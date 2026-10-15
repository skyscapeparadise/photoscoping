#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ps::app {

// The mattes tools/matte_clip.py writes one frame at a time, as 16-bit PNGs
// named by frame index, read while the run that writes them is still going.
//
// Slower to show than the finished matte video -- a PNG is decoded on the CPU
// every time the frame changes -- but there from the moment each frame is
// done, which the video is not until the whole clip is.
class MatteFrames {
public:
    MatteFrames() = default;
    ~MatteFrames();
    MatteFrames(const MatteFrames&) = delete;
    MatteFrames& operator=(const MatteFrames&) = delete;

    void open(SDL_GPUDevice* device, const std::string& dir, std::int64_t frame_count);
    void destroy();

    // Looks at the directory again. True when anything changed.
    bool rescan();

    // Which frames have a matte, one entry per frame of the clip.
    const std::vector<bool>& done() const { return done_; }
    std::int64_t done_count() const { return done_count_; }
    bool has(std::int64_t frame) const;
    // When the newest of them was written; the epoch when there are none.
    std::filesystem::file_time_type newest() const { return newest_; }

    // The frame's matte as a texture, coverage in red; null without one.
    // Decodes only when the frame differs from the last one asked for.
    SDL_GPUTexture* texture(std::int64_t frame);

private:
    bool upload(SDL_Surface* rgba);

    SDL_GPUDevice* device_ = nullptr;
    std::string dir_;
    std::vector<bool> done_;
    std::int64_t done_count_ = 0;
    std::filesystem::file_time_type newest_{};

    SDL_GPUTexture* texture_ = nullptr;
    SDL_GPUTransferBuffer* transfer_ = nullptr;
    int width_ = 0, height_ = 0;
    std::int64_t loaded_ = -1;
};

}  // namespace ps::app

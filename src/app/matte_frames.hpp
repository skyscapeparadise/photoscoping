#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <mutex>
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

    // A second folder of the same frames that, where it has one, is shown
    // instead: the mattes refined by the clip's clean plate. Empty for none.
    void set_preferred(const std::string& dir);
    // Which frames the preferred folder has, and how many.
    const std::vector<bool>& refined() const { return refined_; }
    std::int64_t refined_count() const { return refined_count_; }
    // Whether the frame on screen came from the preferred folder.
    bool shown_refined() const { return shown_refined_; }
    // Whether the frame on screen is not the best there is for it: a refined
    // one has landed since it was read.
    bool shown_outdated(std::int64_t frame) const {
        return shown_ == frame && !shown_refined_ && frame >= 0 &&
               frame < static_cast<std::int64_t>(refined_.size()) && refined_[static_cast<std::size_t>(frame)];
    }

    // Looks at the directories again. True when anything changed.
    bool rescan();

    // Which frames have a matte, one entry per frame of the clip.
    const std::vector<bool>& done() const { return done_; }
    std::int64_t done_count() const { return done_count_; }
    bool matted(std::int64_t frame) const;
    // When the newest of them was written; the epoch when there are none.
    std::filesystem::file_time_type newest() const { return newest_; }

    // The shown frame's matte, coverage in red.
    SDL_GPUTexture* texture() const { return shown_ >= 0 ? texture_ : nullptr; }

    // Whether to keep the shown frame's coverage on the CPU as well, for
    // what is worked out from it there (SubjectEstimate). Off, a frame costs
    // no more than its upload.
    void set_keep_coverage(bool keep) { keep_coverage_ = keep; }
    // The shown frame's coverage, a byte a pixel, if kept: width x height.
    const std::vector<std::uint8_t>& coverage() const { return coverage_; }
    int width() const { return width_; }
    int height() const { return height_; }

protected:
    bool upload(SDL_GPUCommandBuffer* cmd) override;

private:
    // Decoded on the worker, uploaded from on the main thread.
    SDL_Surface* loaded_ = nullptr;
    bool loaded_refined_ = false;
    bool shown_refined_ = false;

    SDL_GPUDevice* device_ = nullptr;
    std::string dir_;
    // Read by the worker, so changed under the lock.
    mutable std::mutex preferred_mutex_;
    std::string preferred_;
    std::vector<bool> refined_;
    std::int64_t refined_count_ = 0;
    std::vector<bool> done_;
    std::int64_t done_count_ = 0;
    std::filesystem::file_time_type newest_{};

    SDL_GPUTexture* texture_ = nullptr;
    SDL_GPUTransferBuffer* transfer_ = nullptr;
    int width_ = 0, height_ = 0;
    bool keep_coverage_ = false;
    std::vector<std::uint8_t> coverage_;
};

}  // namespace ps::app

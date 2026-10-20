#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "app/frame_source.hpp"
#include "app/mask_texture.hpp"
#include "fixes/fixes.hpp"
#include "fixes/rematte.hpp"

namespace ps::app {

// The re-matte patches the clip's boxes want, as the viewer shows them: each
// frame's, read on the worker and folded into one texture over the part of
// the frame they cover -- red the patches' alpha, green how much of it to
// take, which feathers in from each box's edge.
//
// Which patches a frame wants follows the boxes (set_fixes); which of those
// are made follows the directory the re-matte run writes them into
// (rescan). A frame is only fetched for the patches that are both.
class RemattePatches : public FrameSource {
public:
    RemattePatches() = default;
    ~RemattePatches() override;
    RemattePatches(const RemattePatches&) = delete;
    RemattePatches& operator=(const RemattePatches&) = delete;

    void open(SDL_GPUDevice* device, const std::string& dir, int frame_w, int frame_h,
              std::int64_t frame_count);
    void destroy();

    // The boxes changed, or the directory did. Either way the frame on
    // screen is fetched again if what it shows changed.
    void set_fixes(const fixes::Fixes& fixes);
    bool rescan();

    // Whether `frame` has made patches to show.
    bool wants(std::int64_t frame) const;
    bool made(std::int64_t frame, const fixes::PixelRect& box) const;
    // Patches the boxes want, and how many of them are made; and the frames
    // still wanting one.
    std::size_t wanted() const { return jobs_.size(); }
    std::size_t made_count() const { return made_count_; }
    std::size_t frames_to_make() const { return frames_to_make_; }

    // The shown frame's patches, or null; and where they go, in UV.
    SDL_GPUTexture* texture() const { return shown_ >= 0 && has_ ? texture_.texture() : nullptr; }
    void rect_uv(float out[4]) const;

protected:
    bool upload(SDL_GPUCommandBuffer* cmd) override;

private:
    // Rebuilds by_frame_ from the boxes' jobs and the files; refreshes if it
    // changed.
    void relink();

    std::string dir_;
    int frame_w_ = 0, frame_h_ = 0;
    std::int64_t frame_count_ = 0;
    std::vector<fixes::PatchJob> jobs_;
    std::set<std::string> files_;
    std::size_t made_count_ = 0;
    std::size_t frames_to_make_ = 0;

    // Read by the worker.
    mutable std::mutex mutex_;
    std::unordered_map<std::int64_t, std::vector<fixes::PixelRect>> by_frame_;
    // Bumped whenever by_frame_ changes, so a frame the worker was making
    // from the old patches is not shown.
    std::uint64_t generation_ = 0;
    std::uint64_t loaded_generation_ = 0;

    // Made on the worker, uploaded on the main thread.
    std::vector<std::uint8_t> loaded_;
    fixes::PixelRect loaded_rect_;

    MaskTexture texture_;
    fixes::PixelRect rect_;
    bool has_ = false;
};

}  // namespace ps::app

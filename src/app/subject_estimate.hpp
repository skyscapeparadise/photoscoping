#pragma once

#include <SDL3/SDL.h>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "app/mask_texture.hpp"
#include "fixes/fixes.hpp"
#include "fixes/rematte.hpp"

namespace ps::app {

// The subject's own colour, for the subject view of a clip the app matted --
// whose run writes only the matte -- worked out the way an export works it
// out: foreground estimation over the footage with the matte as it stands,
// re-matte patches and fixes and all.
//
// At half the frame's size, on a thread of its own: the estimate is a
// fraction of a second there, which is fine for the frame being looked at and
// too slow to keep up with playback. The colour is straight, not multiplied
// by the matte, so the view composites it with the matte as it is now -- a
// stroke being painted shows at once, its colour catching up when the
// estimate is made again.
class SubjectEstimate {
public:
    struct Inputs {
        std::int64_t frame = -1;
        std::uint64_t version = 0;  // of the matte and fixes it was made from
        int width = 0, height = 0;
        // The footage as the working image holds it: RGBA half floats,
        // linear Rec.2020.
        std::vector<std::uint16_t> working;
        // The stack's matte, a byte a pixel.
        std::vector<std::uint8_t> coverage;
        // The re-matte patches over `patch_rect`, as RemattePatches folds
        // them: red alpha, green weight, four bytes a pixel. Empty for none.
        std::vector<std::uint8_t> patches;
        fixes::PixelRect patch_rect;
        fixes::Fixes fixes;
    };

    SubjectEstimate() = default;
    ~SubjectEstimate();
    SubjectEstimate(const SubjectEstimate&) = delete;
    SubjectEstimate& operator=(const SubjectEstimate&) = delete;

    // `notify` is called on the worker when an estimate is made, to wake the
    // main loop.
    void create(SDL_GPUDevice* device, std::function<void()> notify);
    void destroy();

    // Replaces whatever was waiting to be made.
    void request(Inputs inputs);
    // Whether this frame at this version has been asked for already.
    bool asked(std::int64_t frame, std::uint64_t version) const;
    // Uploads a finished estimate. True if there was one.
    bool take(SDL_GPUCommandBuffer* cmd);
    // Blocks until the estimate asked for last is made. For screenshots.
    void wait();

    bool shows(std::int64_t frame, std::uint64_t version) const {
        return shown_frame_ == frame && shown_version_ == version && texture_.texture();
    }
    bool shows_frame(std::int64_t frame) const {
        return shown_frame_ == frame && texture_.texture();
    }
    // Straight colour, linear Rec.2020, half size.
    SDL_GPUTexture* texture() const { return texture_.texture(); }

    // The work itself: the estimate as RGBA half floats, half size. Exposed
    // for the tests.
    static std::vector<std::uint16_t> estimate(const Inputs& in, int* width, int* height);

private:
    void loop();

    std::function<void()> notify_;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    bool busy_ = false;
    std::optional<Inputs> pending_;
    std::int64_t asked_frame_ = -1;
    std::uint64_t asked_version_ = 0;

    // Made, waiting to be uploaded.
    bool ready_ = false;
    std::vector<std::uint16_t> made_;
    int made_w_ = 0, made_h_ = 0;
    std::int64_t made_frame_ = -1;
    std::uint64_t made_version_ = 0;

    MaskTexture texture_;
    std::int64_t shown_frame_ = -1;
    std::uint64_t shown_version_ = 0;
};

}  // namespace ps::app

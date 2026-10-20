#pragma once

#include <SDL3/SDL.h>

namespace ps::gpu {

class GpuContext;

// How the viewer shows the frame. Must match display.frag.
enum class DisplayView {
    kFootage = 0,
    kMatte = 1,     // the matte, as greys
    kSubject = 2,   // the matted subject over a checkerboard
    kOverlay = 3,   // the footage, washed wherever the matte drops it
};

// Draws the working image into the viewer's area of the linear target, where
// the viewer has placed it: fitted, or zoomed and panned.
class DisplayPass {
public:
    DisplayPass() = default;
    ~DisplayPass();
    DisplayPass(const DisplayPass&) = delete;
    DisplayPass& operator=(const DisplayPass&) = delete;

    bool create(const GpuContext& gpu);
    void destroy();

    // The textures a view reads. `matte` and `subject` are working images of
    // the same frame; a view that needs one which is null falls back to the
    // footage rather than showing something wrong.
    struct Frame {
        SDL_GPUTexture* working = nullptr;
        Uint32 width = 0, height = 0;
        SDL_GPUTexture* matte = nullptr;
        SDL_GPUTexture* subject = nullptr;
        // What the fixes do to the matte (fixes/mask.hpp); null when none.
        SDL_GPUTexture* fixes = nullptr;
    };

    // Records a pass that clears `target` to `background` (linear nits,
    // premultiplied) and draws `frame` (no working image: background only)
    // into `image`, clipped to `area`; both in device pixels, `image` free to
    // run past `area` when zoomed in. Opens the frame, so it clears rather
    // than loads. `scale` is device pixels per logical pixel, which the
    // checkerboard is measured in. `nearest` samples without filtering, so
    // magnified pixels read as squares.
    void draw(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, const SDL_FRect& area,
              const SDL_FRect& image, const Frame& frame, DisplayView view, float scale,
              bool nearest, SDL_FColor background);

private:
    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUGraphicsPipeline* pipeline_ = nullptr;
    SDL_GPUSampler* sampler_ = nullptr;
    SDL_GPUSampler* nearest_ = nullptr;
};

}  // namespace ps::gpu

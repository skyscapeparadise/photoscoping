#pragma once

#include <SDL3/SDL.h>

namespace ps::gpu {

class GpuContext;

// The inspector panel's backdrop: a procedural nebula, purple falling to
// black and slowly churning, drawn into the panel's area of the linear target before the
// interface composites over it.
//
// A shader rather than an image so it is sharp at any panel size and on any
// display, and so resizing the panel uncovers more of the same sky rather than
// stretching a picture of it.
class NebulaPass {
public:
    NebulaPass() = default;
    ~NebulaPass();
    NebulaPass(const NebulaPass&) = delete;
    NebulaPass& operator=(const NebulaPass&) = delete;

    bool create(const GpuContext& gpu);
    void destroy();

    // `area` is the part of the panel on screen, in device pixels, and
    // `logical_w`/`logical_h` the same in logical pixels -- the nebula's
    // features are scaled against those, so they are the same size on a
    // Retina display as on a 1x one. `panel_w` is the panel's full logical
    // width, which is more than `logical_w` while it slides in or out.
    // `time` is how far it has churned, in seconds. Loads the target: the
    // footage pass has already opened the frame.
    void draw(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, const SDL_FRect& area,
              float logical_w, float logical_h, float panel_w, float time);

private:
    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUGraphicsPipeline* pipeline_ = nullptr;
};

}  // namespace ps::gpu

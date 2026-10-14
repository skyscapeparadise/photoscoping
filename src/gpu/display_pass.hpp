#pragma once

#include <SDL3/SDL.h>

namespace ps::gpu {

class GpuContext;

// Draws the working image into the viewer's area of the linear target,
// letterboxed to keep its aspect.
class DisplayPass {
public:
    DisplayPass() = default;
    ~DisplayPass();
    DisplayPass(const DisplayPass&) = delete;
    DisplayPass& operator=(const DisplayPass&) = delete;

    bool create(const GpuContext& gpu);
    void destroy();

    // Records a pass that clears `target` to `background` (linear nits,
    // premultiplied) and draws `working` (null: background only) fitted inside
    // `area`, which is in device pixels. Opens the frame, so it clears rather
    // than loads.
    void draw(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, const SDL_FRect& area,
              SDL_GPUTexture* working, Uint32 work_w, Uint32 work_h, SDL_FColor background);

private:
    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUGraphicsPipeline* pipeline_ = nullptr;
    SDL_GPUSampler* sampler_ = nullptr;
};

}  // namespace ps::gpu

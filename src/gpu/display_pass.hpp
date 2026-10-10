#pragma once

#include <SDL3/SDL.h>

namespace ps::gpu {

class GpuContext;

// Draws the working image into the window, letterboxed to keep its aspect.
class DisplayPass {
public:
    DisplayPass() = default;
    ~DisplayPass();
    DisplayPass(const DisplayPass&) = delete;
    DisplayPass& operator=(const DisplayPass&) = delete;

    bool create(const GpuContext& gpu);
    void destroy();

    // Records a pass that clears the swapchain to `background` and draws
    // `working` (null: background only) fitted inside it.
    void draw(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* swapchain, Uint32 swap_w, Uint32 swap_h,
              SDL_GPUTexture* working, Uint32 work_w, Uint32 work_h, SDL_FColor background);

private:
    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUGraphicsPipeline* pipeline_ = nullptr;
    SDL_GPUSampler* sampler_ = nullptr;
    bool extended_ = false;
};

}  // namespace ps::gpu

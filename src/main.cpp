#include <SDL3/SDL.h>

#include <cmath>

#include "gpu/gpu_context.hpp"

namespace {

// The window background, as an sRGB-encoded colour.
constexpr float kBackground[3] = {0.075f, 0.078f, 0.090f};

float srgb_to_linear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

// The SDR swapchain is a plain UNORM format that takes encoded values as-is;
// the extended linear one wants linear light. Same colour, two spellings.
SDL_FColor background_for(ps::gpu::ColorMode mode) {
    if (mode == ps::gpu::ColorMode::kSDR)
        return {kBackground[0], kBackground[1], kBackground[2], 1.0f};
    return {srgb_to_linear(kBackground[0]), srgb_to_linear(kBackground[1]),
            srgb_to_linear(kBackground[2]), 1.0f};
}

}  // namespace

int main(int, char**) {
    SDL_SetAppMetadata("photoscoping", PHOTOSCOPING_VERSION, "com.rain.photoscoping");
    SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_CREATOR_STRING, "rain multimedia");
    SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_COPYRIGHT_STRING,
                               "copyright © 2026 rain multimedia. all rights reserved.");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    ps::gpu::GpuContext gpu;
    if (!gpu.create("photoscoping", 1440, 900, ps::gpu::ColorMode::kEDR)) {
        SDL_Quit();
        return 1;
    }
    SDL_Log("photoscoping %s\n%s", PHOTOSCOPING_VERSION, gpu.describe().c_str());

    const SDL_FColor clear = background_for(gpu.mode());

    bool running = true;
    while (running) {
        SDL_Event event;
        // Block for input rather than spinning: until there is footage playing,
        // nothing changes on screen without an event.
        if (SDL_WaitEvent(&event)) {
            do {
                if (event.type == SDL_EVENT_QUIT) running = false;
            } while (SDL_PollEvent(&event));
        }

        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu.device());
        if (!cmd) {
            SDL_Log("SDL_AcquireGPUCommandBuffer failed: %s", SDL_GetError());
            break;
        }
        SDL_GPUTexture* swapchain = nullptr;
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, gpu.window(), &swapchain,
                                                   nullptr, nullptr)) {
            SDL_Log("SDL_WaitAndAcquireGPUSwapchainTexture failed: %s", SDL_GetError());
            SDL_CancelGPUCommandBuffer(cmd);
            break;
        }
        // Null without an error when the window is minimized; nothing to draw.
        if (swapchain) {
            SDL_GPUColorTargetInfo target = {};
            target.texture = swapchain;
            target.clear_color = clear;
            target.load_op = SDL_GPU_LOADOP_CLEAR;
            target.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
            SDL_EndGPURenderPass(pass);
        }
        SDL_SubmitGPUCommandBuffer(cmd);
    }

    gpu.destroy();
    SDL_Quit();
    return 0;
}

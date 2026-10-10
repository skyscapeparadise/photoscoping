#pragma once

#include <SDL3/SDL.h>

namespace ps::gpu {

// A pipeline that draws one quad (src/gpu/shaders/quad.vert, no vertex
// buffer) with `fragment_msl` into a single colour target of `format`.
SDL_GPUGraphicsPipeline* create_quad_pipeline(SDL_GPUDevice* device, const char* fragment_msl,
                                              Uint32 fragment_samplers,
                                              SDL_GPUTextureFormat format);

SDL_GPUSampler* create_sampler(SDL_GPUDevice* device, SDL_GPUFilter filter);

// Quad placement for quad.vert: x0, y0, x1, y1 in NDC.
struct QuadRect {
    float rect[4] = {-1.0f, -1.0f, 1.0f, 1.0f};
};

// Draws the quad. Uniform slot 0 of the vertex stage takes `rect`.
void draw_quad(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass, const QuadRect& rect);

}  // namespace ps::gpu

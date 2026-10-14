#pragma once

#include <SDL3/SDL.h>

namespace ps::gpu {

// Uploads straight RGBA8 pixels, tightly packed. Caller owns the result and
// must SDL_ReleaseGPUTexture it.
SDL_GPUTexture* upload_rgba_texture(SDL_GPUDevice* device, const void* pixels,
                                    int width, int height);

// Rasterizes an SVG at an explicit pixel size and uploads it.
//
// Rasterized at the size it will be drawn rather than scaled from a default
// rasterization, which is the difference between crisp vector art and soft
// art. Passing 0 for one dimension lets the drawing's own aspect decide it;
// `out_w`/`out_h` report what that came to.
SDL_GPUTexture* load_svg_texture(SDL_GPUDevice* device, const char* path,
                                 int width, int height, int* out_w = nullptr,
                                 int* out_h = nullptr);

}  // namespace ps::gpu

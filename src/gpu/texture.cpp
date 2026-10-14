#include "gpu/texture.hpp"

#include <SDL3_image/SDL_image.h>

namespace ps::gpu {

SDL_GPUTexture* upload_rgba_texture(SDL_GPUDevice* device, const void* pixels,
                                    int width, int height) {
    if (!device || !pixels || width <= 0 || height <= 0) return nullptr;

    SDL_GPUTextureCreateInfo info = {};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = static_cast<Uint32>(width);
    info.height = static_cast<Uint32>(height);
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;

    SDL_GPUTexture* texture = SDL_CreateGPUTexture(device, &info);
    if (!texture) {
        SDL_Log("SDL_CreateGPUTexture failed: %s", SDL_GetError());
        return nullptr;
    }

    const Uint32 bytes = static_cast<Uint32>(width) * static_cast<Uint32>(height) * 4;
    SDL_GPUTransferBufferCreateInfo tb = {};
    tb.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tb.size = bytes;
    SDL_GPUTransferBuffer* staging = SDL_CreateGPUTransferBuffer(device, &tb);
    if (!staging) {
        SDL_Log("SDL_CreateGPUTransferBuffer failed: %s", SDL_GetError());
        SDL_ReleaseGPUTexture(device, texture);
        return nullptr;
    }

    if (void* mapped = SDL_MapGPUTransferBuffer(device, staging, false)) {
        SDL_memcpy(mapped, pixels, bytes);
        SDL_UnmapGPUTransferBuffer(device, staging);
    }

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src = {};
    src.transfer_buffer = staging;
    src.pixels_per_row = static_cast<Uint32>(width);
    src.rows_per_layer = static_cast<Uint32>(height);
    SDL_GPUTextureRegion dst = {};
    dst.texture = texture;
    dst.w = static_cast<Uint32>(width);
    dst.h = static_cast<Uint32>(height);
    dst.d = 1;
    SDL_UploadToGPUTexture(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);

    SDL_ReleaseGPUTransferBuffer(device, staging);
    return texture;
}

SDL_GPUTexture* load_svg_texture(SDL_GPUDevice* device, const char* path,
                                 int width, int height, int* out_w, int* out_h) {
    SDL_IOStream* io = SDL_IOFromFile(path, "rb");
    if (!io) {
        SDL_Log("cannot open %s: %s", path, SDL_GetError());
        return nullptr;
    }

    SDL_Surface* svg = IMG_LoadSizedSVG_IO(io, width, height);
    SDL_CloseIO(io);
    if (!svg) {
        SDL_Log("IMG_LoadSizedSVG_IO failed for %s: %s", path, SDL_GetError());
        return nullptr;
    }

    // The rasterizer's output format is not guaranteed; convert rather than
    // assume, so a format change upstream cannot silently swizzle the icons.
    SDL_Surface* rgba = svg;
    if (svg->format != SDL_PIXELFORMAT_ABGR8888) {
        rgba = SDL_ConvertSurface(svg, SDL_PIXELFORMAT_ABGR8888);
        SDL_DestroySurface(svg);
        if (!rgba) {
            SDL_Log("SDL_ConvertSurface failed for %s: %s", path, SDL_GetError());
            return nullptr;
        }
    }

    if (out_w) *out_w = rgba->w;
    if (out_h) *out_h = rgba->h;
    SDL_GPUTexture* texture =
        upload_rgba_texture(device, rgba->pixels, rgba->w, rgba->h);
    SDL_DestroySurface(rgba);
    return texture;
}

}  // namespace ps::gpu

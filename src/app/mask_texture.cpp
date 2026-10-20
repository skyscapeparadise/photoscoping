#include "app/mask_texture.hpp"

#include <cstring>

namespace ps::app {

void MaskTexture::destroy() {
    if (device_) {
        if (texture_) SDL_ReleaseGPUTexture(device_, texture_);
        if (transfer_) SDL_ReleaseGPUTransferBuffer(device_, transfer_);
    }
    texture_ = nullptr;
    transfer_ = nullptr;
    width_ = height_ = 0;
}

bool MaskTexture::upload(SDL_GPUCommandBuffer* cmd, const std::vector<std::uint8_t>& values,
                         int width, int height) {
    if (!device_ || values.size() != static_cast<std::size_t>(width) * height) return false;
    if (!texture_ || width != width_ || height != height_) {
        destroy();
        SDL_GPUTextureCreateInfo info = {};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = static_cast<Uint32>(width);
        info.height = static_cast<Uint32>(height);
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        texture_ = SDL_CreateGPUTexture(device_, &info);
        SDL_GPUTransferBufferCreateInfo tb = {};
        tb.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tb.size = static_cast<Uint32>(values.size());
        transfer_ = SDL_CreateGPUTransferBuffer(device_, &tb);
        if (!texture_ || !transfer_) {
            SDL_Log("cannot make the mask texture: %s", SDL_GetError());
            destroy();
            return false;
        }
        width_ = width;
        height_ = height;
    }
    void* mapped = SDL_MapGPUTransferBuffer(device_, transfer_, /*cycle=*/true);
    if (!mapped) return false;
    std::memcpy(mapped, values.data(), values.size());
    SDL_UnmapGPUTransferBuffer(device_, transfer_);

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src = {};
    src.transfer_buffer = transfer_;
    src.pixels_per_row = static_cast<Uint32>(width);
    src.rows_per_layer = static_cast<Uint32>(height);
    SDL_GPUTextureRegion dst = {};
    dst.texture = texture_;
    dst.w = static_cast<Uint32>(width);
    dst.h = static_cast<Uint32>(height);
    dst.d = 1;
    SDL_UploadToGPUTexture(copy, &src, &dst, /*cycle=*/true);
    SDL_EndGPUCopyPass(copy);
    return true;
}

}  // namespace ps::app

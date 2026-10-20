#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <vector>

namespace ps::app {

// A texture refilled from the CPU: the fixes' mask, which changes when they
// do or the frame does. Four bytes a pixel unless made otherwise.
class MaskTexture {
public:
    MaskTexture() = default;
    ~MaskTexture() { destroy(); }
    MaskTexture(const MaskTexture&) = delete;
    MaskTexture& operator=(const MaskTexture&) = delete;

    void create(SDL_GPUDevice* device,
                SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                int bytes_per_pixel = 4) {
        device_ = device;
        format_ = format;
        bytes_per_pixel_ = bytes_per_pixel;
    }
    void destroy();

    // Records the upload into `cmd`, ahead of whatever samples it. `values`
    // is width x height x 4.
    bool upload(SDL_GPUCommandBuffer* cmd, const std::vector<std::uint8_t>& values, int width,
                int height) {
        return upload(cmd, values.data(), values.size(), width, height);
    }
    // `size` bytes, width x height pixels of the texture's format.
    bool upload(SDL_GPUCommandBuffer* cmd, const void* data, std::size_t size, int width, int height);
    SDL_GPUTexture* texture() const { return texture_; }

private:
    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUTexture* texture_ = nullptr;
    SDL_GPUTransferBuffer* transfer_ = nullptr;
    int width_ = 0, height_ = 0;
    SDL_GPUTextureFormat format_ = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    int bytes_per_pixel_ = 4;
};

}  // namespace ps::app

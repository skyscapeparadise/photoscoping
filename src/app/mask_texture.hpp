#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <vector>

namespace ps::app {

// A texture refilled from the CPU: the fixes' mask, which changes when they
// do or the frame does. Four bytes a pixel.
class MaskTexture {
public:
    MaskTexture() = default;
    ~MaskTexture() { destroy(); }
    MaskTexture(const MaskTexture&) = delete;
    MaskTexture& operator=(const MaskTexture&) = delete;

    void create(SDL_GPUDevice* device) { device_ = device; }
    void destroy();

    // Records the upload into `cmd`, ahead of whatever samples it. `values`
    // is width x height x 4.
    bool upload(SDL_GPUCommandBuffer* cmd, const std::vector<std::uint8_t>& values, int width,
                int height);
    SDL_GPUTexture* texture() const { return texture_; }

private:
    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUTexture* texture_ = nullptr;
    SDL_GPUTransferBuffer* transfer_ = nullptr;
    int width_ = 0, height_ = 0;
};

}  // namespace ps::app

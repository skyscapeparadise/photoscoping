#pragma once

#include <SDL3/SDL.h>
#include <string>

namespace ps::gpu {

// How the swapchain is composited.
//
// kEDR is macOS's own model and the one to prefer there: extended linear sRGB
// where 1.0 is the system's SDR white, so the interface sits at the brightness
// the user chose and highlights in the footage use the headroom above it.
// kSDR is the universal baseline and needs no capability query.
enum class ColorMode { kSDR, kEDR };

// Owns the window, the SDL_GPU device and the swapchain configuration.
class GpuContext {
public:
    GpuContext() = default;
    ~GpuContext();

    GpuContext(const GpuContext&) = delete;
    GpuContext& operator=(const GpuContext&) = delete;

    // `preferred` is a request, not a guarantee: asking for kEDR on a display
    // that cannot present it falls back to kSDR rather than failing. Check
    // mode() afterwards for what was actually selected.
    bool create(const char* title, int width, int height, ColorMode preferred);
    void destroy();

    SDL_Window* window() const { return window_; }
    SDL_GPUDevice* device() const { return device_; }
    ColorMode mode() const { return mode_; }
    SDL_GPUTextureFormat swapchain_format() const { return swapchain_format_; }

    // Printed at startup. A wrong composition or format is silent and
    // expensive to debug later, so it is surfaced deliberately.
    std::string describe() const;

private:
    SDL_Window* window_ = nullptr;
    SDL_GPUDevice* device_ = nullptr;
    ColorMode mode_ = ColorMode::kSDR;
    SDL_GPUTextureFormat swapchain_format_ = SDL_GPU_TEXTUREFORMAT_INVALID;
};

// Format names for diagnostics. SDL has no public stringifier for these.
const char* texture_format_name(SDL_GPUTextureFormat format);

}  // namespace ps::gpu

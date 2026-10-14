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

// Owns the window, the SDL_GPU device, the swapchain configuration, and the
// linear-light offscreen target the window composites into.
//
// Two stages: the footage, the panel backdrop and the interface all write
// linear light into one float target, and a single final pass encodes it for
// the swapchain. That is why nothing upstream of the encode has to know
// whether the window is EDR or SDR.
class GpuContext {
public:
    GpuContext() = default;
    ~GpuContext();

    GpuContext(const GpuContext&) = delete;
    GpuContext& operator=(const GpuContext&) = delete;

    // `preferred` is a request, not a guarantee: asking for kEDR on a display
    // that cannot present it falls back to kSDR rather than failing. Check
    // mode() afterwards for what was actually selected. A hidden window still
    // gets a device and swapchain, which is what headless probes use.
    bool create(const char* title, int width, int height, ColorMode preferred,
                bool visible = true);
    void destroy();

    // Recreates the linear target when the drawable size changes. A no-op when
    // the size is unchanged, so it is safe to call every frame.
    bool resize_if_needed(Uint32 width, Uint32 height);

    SDL_Window* window() const { return window_; }
    SDL_GPUDevice* device() const { return device_; }
    ColorMode mode() const { return mode_; }
    SDL_GPUTextureFormat swapchain_format() const { return swapchain_format_; }
    SDL_GPUTexture* linear_target() const { return linear_target_; }
    SDL_GPUTextureFormat linear_format() const { return kLinearFormat; }
    Uint32 target_width() const { return target_w_; }
    Uint32 target_height() const { return target_h_; }

    // Printed at startup. A wrong composition or format is silent and
    // expensive to debug later, so it is surfaced deliberately.
    std::string describe() const;

private:
    // Half float: wide enough to composite without banding, and signed and
    // extended enough to hold footage above reference white until the encode.
    static constexpr SDL_GPUTextureFormat kLinearFormat =
        SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;

    SDL_Window* window_ = nullptr;
    SDL_GPUDevice* device_ = nullptr;
    ColorMode mode_ = ColorMode::kSDR;
    SDL_GPUTextureFormat swapchain_format_ = SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUTexture* linear_target_ = nullptr;
    Uint32 target_w_ = 0;
    Uint32 target_h_ = 0;
};

// Format names for diagnostics. SDL has no public stringifier for these.
const char* texture_format_name(SDL_GPUTextureFormat format);

}  // namespace ps::gpu

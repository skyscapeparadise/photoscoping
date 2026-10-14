#include "gpu/gpu_context.hpp"

namespace ps::gpu {

namespace {

// Which shader format we promise to hand SDL_GPU. This is what *we* can
// supply, not what the backend prefers -- Metal only accepts MSL, so asking
// for SPIR-V on macOS fails device creation outright.
constexpr SDL_GPUShaderFormat kShaderFormats =
#if defined(SDL_PLATFORM_APPLE)
    SDL_GPU_SHADERFORMAT_MSL;
#elif defined(SDL_PLATFORM_WIN32)
    SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_SPIRV;
#else
    SDL_GPU_SHADERFORMAT_SPIRV;
#endif

}  // namespace

const char* texture_format_name(SDL_GPUTextureFormat format) {
    switch (format) {
        case SDL_GPU_TEXTUREFORMAT_INVALID:              return "INVALID";
        case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM:       return "B8G8R8A8_UNORM";
        case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM:       return "R8G8B8A8_UNORM";
        case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB:  return "B8G8R8A8_UNORM_SRGB";
        case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB:  return "R8G8B8A8_UNORM_SRGB";
        case SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM:    return "R10G10B10A2_UNORM";
        case SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT:   return "R16G16B16A16_FLOAT";
        default:                                         return "other";
    }
}

GpuContext::~GpuContext() { destroy(); }

bool GpuContext::create(const char* title, int width, int height, ColorMode preferred,
                        bool visible) {
    window_ = SDL_CreateWindow(title, width, height,
                               SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                   (visible ? 0 : SDL_WINDOW_HIDDEN));
    if (!window_) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }
    SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

    // debug_mode=true costs validation overhead but catches malformed pipeline
    // and binding state at the call that caused it rather than as a corrupted
    // frame several passes later. Worth it until the renderer is stable.
    device_ = SDL_CreateGPUDevice(kShaderFormats, /*debug_mode=*/true, /*name=*/nullptr);
    if (!device_) {
        SDL_Log("SDL_CreateGPUDevice failed: %s", SDL_GetError());
        destroy();
        return false;
    }

    if (!SDL_ClaimWindowForGPUDevice(device_, window_)) {
        SDL_Log("SDL_ClaimWindowForGPUDevice failed: %s", SDL_GetError());
        destroy();
        return false;
    }

    // An SDR display is a normal outcome (an external monitor, say), not an
    // error -- fall back rather than fail.
    const bool edr_supported = SDL_WindowSupportsGPUSwapchainComposition(
        device_, window_, SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR);
    mode_ = (preferred == ColorMode::kEDR && edr_supported) ? ColorMode::kEDR
                                                            : ColorMode::kSDR;

    const SDL_GPUSwapchainComposition composition =
        (mode_ == ColorMode::kEDR) ? SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR
                                   : SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
    if (!SDL_SetGPUSwapchainParameters(device_, window_, composition,
                                       SDL_GPU_PRESENTMODE_VSYNC)) {
        SDL_Log("SDL_SetGPUSwapchainParameters failed: %s", SDL_GetError());
        destroy();
        return false;
    }

    // Queried, never hardcoded, and only valid AFTER SetGPUSwapchainParameters:
    // the real format is a function of the active composition.
    swapchain_format_ = SDL_GetGPUSwapchainTextureFormat(device_, window_);
    if (swapchain_format_ == SDL_GPU_TEXTUREFORMAT_INVALID) {
        SDL_Log("SDL_GetGPUSwapchainTextureFormat returned INVALID");
        destroy();
        return false;
    }

    // Without this the window can open behind whatever was frontmost, and an
    // inactive app receives no mouse-motion events at all -- hover states
    // simply never fire, which looks like a UI bug rather than a focus one.
    if (visible) SDL_RaiseWindow(window_);
    return true;
}

bool GpuContext::resize_if_needed(Uint32 width, Uint32 height) {
    if (width == 0 || height == 0) return false;
    if (linear_target_ && width == target_w_ && height == target_h_) return true;
    if (linear_target_) SDL_ReleaseGPUTexture(device_, linear_target_);

    SDL_GPUTextureCreateInfo info = {};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = kLinearFormat;
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    linear_target_ = SDL_CreateGPUTexture(device_, &info);
    if (!linear_target_) {
        SDL_Log("SDL_CreateGPUTexture (linear target) failed: %s", SDL_GetError());
        target_w_ = target_h_ = 0;
        return false;
    }
    target_w_ = width;
    target_h_ = height;
    return true;
}

void GpuContext::destroy() {
    if (device_ && linear_target_) SDL_ReleaseGPUTexture(device_, linear_target_);
    linear_target_ = nullptr;
    target_w_ = target_h_ = 0;
    if (device_ && window_) {
        SDL_ReleaseWindowFromGPUDevice(device_, window_);
    }
    if (device_) {
        SDL_DestroyGPUDevice(device_);
        device_ = nullptr;
    }
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
}

std::string GpuContext::describe() const {
    const char* driver = device_ ? SDL_GetGPUDeviceDriver(device_) : "(none)";
    std::string s;
    s += "  gpu driver       : ";
    s += driver ? driver : "(unknown)";
    s += "\n  composition      : ";
    s += (mode_ == ColorMode::kEDR) ? "HDR_EXTENDED_LINEAR" : "SDR";
    s += "\n  swapchain format : ";
    s += texture_format_name(swapchain_format_);
    s += "\n  hdr headroom     : " +
         std::to_string(SDL_GetFloatProperty(SDL_GetWindowProperties(window_),
                                             SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT, -1.0f));
    return s;
}

}  // namespace ps::gpu

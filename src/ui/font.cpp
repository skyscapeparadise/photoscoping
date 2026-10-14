#include "ui/font.hpp"

#include <algorithm>
#include <cmath>

namespace ps::ui {

FontCache::~FontCache() { destroy(); }

bool FontCache::init(SDL_GPUDevice* device, const char* regular_path,
                     const char* bold_path) {
    destroy();
    if (!TTF_Init()) {
        SDL_Log("TTF_Init failed: %s", SDL_GetError());
        return false;
    }
    engine_ = TTF_CreateGPUTextEngine(device);
    if (!engine_) {
        SDL_Log("TTF_CreateGPUTextEngine failed: %s", SDL_GetError());
        return false;
    }
    path_ = regular_path;
    bold_path_ = bold_path ? bold_path : regular_path;
    return true;
}

TTF_Font* FontCache::font(float device_pixel_size, bool bold) {
    const Key key{std::max(1, static_cast<int>(std::lround(device_pixel_size))), bold};
    auto it = fonts_.find(key);
    if (it != fonts_.end()) return it->second;

    const std::string& path = bold ? bold_path_ : path_;
    TTF_Font* f = TTF_OpenFont(path.c_str(), static_cast<float>(key.size));
    if (!f) {
        SDL_Log("TTF_OpenFont(%s, %d) failed: %s", path.c_str(), key.size,
                SDL_GetError());
        return nullptr;
    }
    // No TTF_STYLE_BOLD: that emboldens the outline it was given, which here
    // would be ExtraLight smeared until it is thick. The bold file is the real
    // wght 700 instance.
    //
    // Light hinting, not the default. Full hinting rounds each advance to a
    // whole pixel, which at 14px renders this font's test line 313px wide
    // against design metrics of 306.6 -- a 2% stretch that differs per string.
    // Light leaves horizontal metrics alone and still grid-fits vertically.
    TTF_SetFontHinting(f, TTF_HINTING_LIGHT);
    if (TTF_Font* fb = fallback(key.size)) TTF_AddFallbackFont(f, fb);
    fonts_[key] = f;
    return f;
}

TTF_Font* FontCache::fallback(int size) {
    const Key key{size, false};
    auto it = fallbacks_.find(key);
    if (it != fallbacks_.end()) return it->second;

    // Absent on a non-Mac, which only costs the symbols themselves.
    static constexpr const char* kPath = "/System/Library/Fonts/Apple Symbols.ttf";
    TTF_Font* f = TTF_OpenFont(kPath, static_cast<float>(size));
    if (f) TTF_SetFontHinting(f, TTF_HINTING_LIGHT);
    fallbacks_[key] = f;
    return f;
}

void FontCache::destroy() {
    for (auto& [key, f] : fonts_) TTF_CloseFont(f);
    fonts_.clear();
    // Closed after the fonts that reference them.
    for (auto& [key, f] : fallbacks_) if (f) TTF_CloseFont(f);
    fallbacks_.clear();
    if (engine_) {
        TTF_DestroyGPUTextEngine(engine_);
        engine_ = nullptr;
    }
    TTF_Quit();
}

}  // namespace ps::ui

#pragma once

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <map>
#include <string>

namespace ps::ui {

// Fonts and the shared GPU text engine.
//
// SDL3_ttf's GPU text engine does the work a glyph atlas would otherwise mean
// writing: HarfBuzz shaping, rasterization, atlas packing, and caching per
// TTF_Text. It hands back indexed triangles against its own atlas texture,
// which the draw list consumes directly (add_text_geometry).
//
// Fonts are opened at *device* pixel size, not logical. Opening at logical size
// and letting the renderer scale up is the difference between crisp text and
// soft text on a HiDPI display, and it is the same reason icons are rasterized
// at device size.
class FontCache {
public:
    FontCache() = default;
    ~FontCache();

    FontCache(const FontCache&) = delete;
    FontCache& operator=(const FontCache&) = delete;

    // Two real files rather than one variable font. Its default instance is
    // ExtraLight, and there is no axis API in SDL_ttf to move off it, so the
    // weights are instantiated ahead of time: fonts/MonaSans-Regular.ttf and
    // fonts/MonaSans-Bold.ttf, alongside the variable font they came from.
    bool init(SDL_GPUDevice* device, const char* regular_path,
              const char* bold_path);
    void destroy();

    // `device_pixel_size` is logical size multiplied by the display scale.
    // Rounded to whole pixels, since a font opened at 21.7px and one at 21.8px
    // are indistinguishable but would be two separate rasterized sets.
    TTF_Font* font(float device_pixel_size, bool bold);

    TTF_TextEngine* engine() const { return engine_; }
    bool ready() const { return engine_ != nullptr; }

private:
    struct Key {
        int size;
        bool bold;
        bool operator<(const Key& o) const {
            return size != o.size ? size < o.size : bold < o.bold;
        }
    };

    TTF_TextEngine* engine_ = nullptr;
    std::string path_;
    std::string bold_path_;
    std::map<Key, TTF_Font*> fonts_;
    // MonaSans has no keycap symbols -- no command, option, backspace or
    // return glyph -- so a shortcut label would come out as .notdef. This is
    // the one system face that covers them, opened per size alongside the
    // main font.
    std::map<Key, TTF_Font*> fallbacks_;
    TTF_Font* fallback(int size);
};

}  // namespace ps::ui

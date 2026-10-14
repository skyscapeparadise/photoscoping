#include "gpu/icon_atlas.hpp"

#include <SDL3_image/SDL_image.h>

#include <algorithm>
#include <cstring>

#include "gpu/texture.hpp"

namespace ps::gpu {

namespace {

// Transparent gutter between entries. Without it, linear filtering at a
// sub-rect edge samples the neighbour and icons pick up a sliver of whatever
// was packed next to them.
constexpr int kPadding = 2;

std::string stem_of(const char* filename) {
    std::string s(filename);
    const std::size_t dot = s.find_last_of('.');
    return (dot == std::string::npos) ? s : s.substr(0, dot);
}

struct Rasterized {
    std::string name;
    SDL_Surface* surface = nullptr;
};

}  // namespace

IconAtlas::~IconAtlas() { destroy(); }

bool IconAtlas::build(SDL_GPUDevice* device, const char* dir, int height_px) {
    destroy();
    device_ = device;
    if (!device || height_px <= 0) return false;

    int file_count = 0;
    char** files = SDL_GlobDirectory(dir, "*.svg", SDL_GLOB_CASEINSENSITIVE, &file_count);
    if (!files || file_count == 0) {
        SDL_Log("IconAtlas: no svgs found in %s", dir);
        if (files) SDL_free(files);
        return false;
    }

    std::vector<Rasterized> items;
    items.reserve(static_cast<std::size_t>(file_count));

    for (int i = 0; i < file_count; ++i) {
        const std::string path = std::string(dir) + "/" + files[i];
        SDL_IOStream* io = SDL_IOFromFile(path.c_str(), "rb");
        if (!io) continue;

        // width 0 => auto-size from the source aspect ratio.
        SDL_Surface* raw = IMG_LoadSizedSVG_IO(io, 0, height_px);
        SDL_CloseIO(io);
        if (!raw) {
            SDL_Log("IconAtlas: failed to rasterize %s: %s", path.c_str(), SDL_GetError());
            continue;
        }

        SDL_Surface* rgba = raw;
        if (raw->format != SDL_PIXELFORMAT_ABGR8888) {
            rgba = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_ABGR8888);
            SDL_DestroySurface(raw);
            if (!rgba) continue;
        }
        items.push_back(Rasterized{stem_of(files[i]), rgba});
    }
    SDL_free(files);

    if (items.empty()) return false;

    // Shelf packing: tallest first, laid left to right, wrapping to a new row
    // when the current one runs out. Good enough for a fixed icon set -- the
    // wasted space is the ragged right edge and the height variance within a
    // shelf, both small when every icon shares a target height.
    std::vector<std::size_t> order(items.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return items[a].surface->h > items[b].surface->h;
    });

    long total_area = 0;
    int widest = 0;
    for (const Rasterized& it : items) {
        total_area += static_cast<long>(it.surface->w + kPadding) *
                      static_cast<long>(it.surface->h + kPadding);
        widest = std::max(widest, it.surface->w + kPadding);
    }
    // Start from a square-ish sheet, rounded up to a power of two, and never
    // narrower than the widest single icon.
    int sheet_w = 64;
    while (static_cast<long>(sheet_w) * sheet_w < total_area || sheet_w < widest) {
        sheet_w *= 2;
    }

    struct Placement { std::size_t index; int x, y; };
    std::vector<Placement> placements;
    placements.reserve(items.size());

    int pen_x = kPadding, pen_y = kPadding, shelf_h = 0;
    for (std::size_t idx : order) {
        SDL_Surface* s = items[idx].surface;
        if (pen_x + s->w + kPadding > sheet_w) {
            pen_x = kPadding;
            pen_y += shelf_h + kPadding;
            shelf_h = 0;
        }
        placements.push_back(Placement{idx, pen_x, pen_y});
        pen_x += s->w + kPadding;
        shelf_h = std::max(shelf_h, s->h);
    }
    const int sheet_h = pen_y + shelf_h + kPadding;

    sheet_ = SDL_CreateSurface(sheet_w, sheet_h, SDL_PIXELFORMAT_ABGR8888);
    if (!sheet_) {
        SDL_Log("IconAtlas: SDL_CreateSurface failed: %s", SDL_GetError());
        for (Rasterized& it : items) SDL_DestroySurface(it.surface);
        return false;
    }
    SDL_ClearSurface(sheet_, 0.0f, 0.0f, 0.0f, 0.0f);

    for (const Placement& p : placements) {
        SDL_Surface* s = items[p.index].surface;
        SDL_Rect dst{p.x, p.y, s->w, s->h};
        // No blending: copy the source's own alpha rather than compositing it
        // over transparent black, which would premultiply it a second time.
        SDL_SetSurfaceBlendMode(s, SDL_BLENDMODE_NONE);
        SDL_BlitSurface(s, nullptr, sheet_, &dst);

        IconEntry entry;
        entry.uv = Rect{static_cast<float>(p.x) / static_cast<float>(sheet_w),
                        static_cast<float>(p.y) / static_cast<float>(sheet_h),
                        static_cast<float>(s->w) / static_cast<float>(sheet_w),
                        static_cast<float>(s->h) / static_cast<float>(sheet_h)};
        entry.width = s->w;
        entry.height = s->h;
        entry.aspect = static_cast<float>(s->w) / static_cast<float>(s->h);
        entries_[items[p.index].name] = entry;
    }

    for (Rasterized& it : items) SDL_DestroySurface(it.surface);

    texture_ = upload_rgba_texture(device, sheet_->pixels, sheet_w, sheet_h);
    if (!texture_) {
        destroy();
        return false;
    }

    width_ = sheet_w;
    height_ = sheet_h;
    return true;
}

const IconEntry* IconAtlas::find(const std::string& name) const {
    auto it = entries_.find(name);
    return (it == entries_.end()) ? nullptr : &it->second;
}

bool IconAtlas::save_debug_png(const char* path) const {
    if (!sheet_) return false;
    return IMG_SavePNG(sheet_, path);
}

void IconAtlas::destroy() {
    if (texture_ && device_) {
        SDL_ReleaseGPUTexture(device_, texture_);
    }
    texture_ = nullptr;
    if (sheet_) {
        SDL_DestroySurface(sheet_);
        sheet_ = nullptr;
    }
    entries_.clear();
    width_ = height_ = 0;
    device_ = nullptr;
}

}  // namespace ps::gpu

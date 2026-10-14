#pragma once

#include <SDL3/SDL.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "gpu/draw_list.hpp"

namespace ps::gpu {

struct IconEntry {
    // Sub-rectangle within the atlas, 0..1. Hand straight to add_image().
    Rect uv;
    int width = 0;   // rasterized size in atlas pixels
    int height = 0;
    // Natural width/height of the source art. Icons are not square -- lock.svg
    // is 40x56 -- so a caller laying one out at a given height needs this to
    // avoid distorting it.
    float aspect = 1.0f;
};

// Every icon rasterized once and packed into a single texture.
//
// The point is batching: with one texture behind every icon, a panel full of
// them collapses to one draw call instead of one per icon. add_image() already
// takes a UV sub-rect for exactly this.
//
// Built at startup rather than baked at build time. Rasterizing the whole set
// costs a few milliseconds, which does not justify a build step and an asset
// format yet; if that changes, the packing here is what moves offline and the
// lookup API stays as it is.
class IconAtlas {
public:
    IconAtlas() = default;
    ~IconAtlas();

    IconAtlas(const IconAtlas&) = delete;
    IconAtlas& operator=(const IconAtlas&) = delete;

    // Rasterizes every *.svg in `dir` to `height_px` tall, preserving each
    // one's aspect ratio, and packs them. `height_px` should already include
    // the HiDPI factor -- these are device pixels.
    bool build(SDL_GPUDevice* device, const char* dir, int height_px);
    void destroy();

    // Keyed by stem, so "lock.svg" is found as "lock". Null if absent.
    const IconEntry* find(const std::string& name) const;

    SDL_GPUTexture* texture() const { return texture_; }
    std::size_t count() const { return entries_.size(); }
    int width() const { return width_; }
    int height() const { return height_; }

    // Writes the packed sheet out for inspection. Packing bugs are close to
    // invisible in a UI -- an icon sampling a neighbour's edge reads as a
    // rendering artefact -- and obvious the moment you look at the sheet.
    bool save_debug_png(const char* path) const;

private:
    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUTexture* texture_ = nullptr;
    SDL_Surface* sheet_ = nullptr;  // kept only for save_debug_png
    std::unordered_map<std::string, IconEntry> entries_;
    int width_ = 0;
    int height_ = 0;
};

}  // namespace ps::gpu

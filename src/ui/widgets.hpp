#pragma once

#include "gpu/icon_atlas.hpp"
#include "ui/node.hpp"

namespace ps::ui {

// A rounded rectangle. Fill and border animate, because nearly every hover and
// selection state in the interface is a colour change.
class RectNode : public Node {
public:
    Animated<Color> fill{Color{0, 0, 0, 0}};
    Animated<Color> border_color{Color{0, 0, 0, 0}};
    Animated<float> border_width{0.0f};
    float radius = 0.0f;

protected:
    void paint_self(gpu::DrawList& draw, const PaintContext& ctx) override;
    bool tick_self(float dt) override;
};

// An icon or image, with tinting folded in: the draw list treats tinting as a
// mode rather than a second pass.
class IconNode : public Node {
public:
    // Either an atlas entry or a standalone texture. Icons come from the atlas;
    // anything rasterized at display size (the controller body, whose SVG is
    // stretched far past the atlas's rasterization height) supplies its own.
    const gpu::IconAtlas* atlas = nullptr;
    std::string icon_name;
    SDL_GPUTexture* texture = nullptr;

    Animated<Color> tint{Color{1, 1, 1, 1}};
    bool colorize = true;
    float radius = 0.0f;

    // How the art fills the node's box. Stretch is the default here because a node's box is
    // usually already the right shape; Fit matters for glyphs dropped into a
    // square cell, and Crop for photographic content in a fixed card.
    enum class FillMode { kStretch, kFit, kCrop };
    FillMode fill_mode = FillMode::kStretch;

    // Width/height of a standalone texture. Atlas entries carry their own, but
    // nothing else knows the shape of an SDL_GPUTexture, so Fit and Crop need
    // to be told. Left at 0 they degrade to Stretch rather than guessing.
    float source_aspect = 0.0f;

protected:
    void paint_self(gpu::DrawList& draw, const PaintContext& ctx) override;
    bool tick_self(float dt) override;
};

}  // namespace ps::ui

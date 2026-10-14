#include "ui/widgets.hpp"

#include <algorithm>

namespace ps::ui {

void RectNode::paint_self(gpu::DrawList& draw, const PaintContext& ctx) {
    gpu::RectStyle style;
    style.fill = fill.value();
    style.border = border_color.value();
    style.radius = radius;
    style.border_width = border_width.value();

    // Inherited opacity multiplies in here rather than being a separate draw
    // list concept, so a faded parent fades its whole subtree.
    style.fill.a *= ctx.opacity;
    style.border.a *= ctx.opacity;

    if (style.fill.a <= 0.0f && (style.border.a <= 0.0f || style.border_width <= 0.0f)) {
        return;  // fully transparent, with no border
    }
    draw.add_rect(Rect{ctx.origin_x, ctx.origin_y, width, height}, style);
}

bool RectNode::tick_self(float dt) {
    bool animating = fill.tick(dt);
    animating |= border_color.tick(dt);
    animating |= border_width.tick(dt);
    return animating;
}

void IconNode::paint_self(gpu::DrawList& draw, const PaintContext& ctx) {
    SDL_GPUTexture* tex = texture;
    Rect uv{0.0f, 0.0f, 1.0f, 1.0f};
    float aspect = 1.0f;

    if (!tex && atlas) {
        const gpu::IconEntry* entry = atlas->find(icon_name);
        if (!entry) return;
        tex = atlas->texture();
        uv = entry->uv;
        aspect = entry->aspect;
    }
    if (!tex) return;
    if (source_aspect > 0.0f) aspect = source_aspect;

    Rect box{ctx.origin_x, ctx.origin_y, width, height};
    const float box_aspect = (height > 0.0f) ? width / height : 1.0f;
    if (fill_mode == FillMode::kFit && aspect > 0.0f) {
        // Fit inside the box and centre, which is what PreserveAspectFit does.
        if (aspect > box_aspect) {
            const float h = width / aspect;
            box.y += (height - h) * 0.5f;
            box.h = h;
        } else {
            const float w = height * aspect;
            box.x += (width - w) * 0.5f;
            box.w = w;
        }
    } else if (fill_mode == FillMode::kCrop && aspect > 0.0f) {
        // PreserveAspectCrop: fill the box and take the centre of whichever
        // axis overhangs. The box is untouched; the source rectangle shrinks.
        if (aspect > box_aspect) {
            const float span = box_aspect / aspect;
            uv.x += uv.w * (1.0f - span) * 0.5f;
            uv.w *= span;
        } else {
            const float span = aspect / box_aspect;
            uv.y += uv.h * (1.0f - span) * 0.5f;
            uv.h *= span;
        }
    }

    gpu::ImageStyle style;
    style.tint = tint.value();
    style.tint.a *= ctx.opacity;
    style.radius = radius;
    style.colorize = colorize;
    if (style.tint.a <= 0.0f) return;

    draw.add_image(box, tex, uv, style);
}

bool IconNode::tick_self(float dt) { return tint.tick(dt); }

}  // namespace ps::ui

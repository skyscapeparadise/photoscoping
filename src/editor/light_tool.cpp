#include "editor/light_tool.hpp"

#include <algorithm>
#include <cmath>

#include "editor/lines.hpp"
#include "editor/theme.hpp"

namespace ps::editor {

namespace {

constexpr float kReach = 9.0f;  // logical pixels from a corner
constexpr float kHandle = 8.0f;

// Even-odd: whether (x, y) is inside the polygon.
bool inside(const std::vector<float>& xy, float x, float y) {
    bool in = false;
    const std::size_t n = xy.size() / 2;
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        const float xi = xy[i * 2], yi = xy[i * 2 + 1], xj = xy[j * 2], yj = xy[j * 2 + 1];
        if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) in = !in;
    }
    return in;
}

}  // namespace

void LightTool::attach(Viewer* viewer, fixes::Fixes* fixes) {
    viewer_ = viewer;
    fixes_ = fixes;
    anchors = ui::Anchors::Fill();
    clip = true;
    visible = false;
    keeps_drag = true;
    on_press = [this](float x, float y) { press(x, y); };
    on_drag = [this](float x, float y) { drag(x, y); };
    on_release = [this] { release(); };
    on_middle_press = [this](float x, float y) { viewer_->begin_pan(x, y); };
    on_middle_release = [this] { viewer_->end_pan(); };
}

void LightTool::set_active(bool active) {
    visible = active;
    if (!active) drag_ = Drag::kNone;
}

std::string LightTool::heading() const {
    const fixes::LightCrop* crop = fixes_->find_light(selected_);
    return crop ? crop->name : "light map";
}

void LightTool::select(int id) {
    if (selected_ == id) return;
    selected_ = id;
    if (on_selection_changed) on_selection_changed();
}

void LightTool::forget_missing() {
    if (selected_ && !fixes_->find_light(selected_)) select(0);
}

int LightTool::hit(float x, float y, int* corner) const {
    auto screen = [&](const fixes::LightCrop& crop) {
        std::vector<float> xy;
        for (const fixes::Point& p : crop.at(frame_)) {
            float sx = 0.0f, sy = 0.0f;
            viewer_->from_image(p.x, p.y, &sx, &sy);
            xy.push_back(sx);
            xy.push_back(sy);
        }
        return xy;
    };
    auto test = [&](const fixes::LightCrop& crop) {
        const std::vector<float> xy = screen(crop);
        for (int k = 0; k < 4; ++k)
            if (std::hypot(xy[k * 2] - x, xy[k * 2 + 1] - y) <= kReach) {
                *corner = k;
                return true;
            }
        if (inside(xy, x, y)) {
            *corner = -1;
            return true;
        }
        return false;
    };
    if (const fixes::LightCrop* sel = fixes_->find_light(selected_); sel && test(*sel)) return sel->id;
    for (auto it = fixes_->lights.rbegin(); it != fixes_->lights.rend(); ++it)
        if (test(*it)) return it->id;
    return 0;
}

void LightTool::press(float x, float y) {
    edited_ = false;
    if (viewer_->space_held()) {
        drag_ = Drag::kPan;
        viewer_->begin_pan(x, y);
        return;
    }
    viewer_->to_image(x, y, &grab_x_, &grab_y_);
    int corner = -1;
    if (const int id = hit(x, y, &corner)) {
        select(id);
        drag_ = corner >= 0 ? Drag::kCorner : Drag::kMove;
        drag_id_ = id;
        corner_ = corner;
        grabbed_ = fixes_->find_light(id)->at(frame_);
        return;
    }
    // A new crop, made once the drag has some size.
    drag_ = Drag::kDraw;
    drag_id_ = 0;
    select(0);
}

void LightTool::drag(float x, float y) {
    if (drag_ == Drag::kPan) {
        viewer_->pan_to(x, y);
        return;
    }
    if (drag_ == Drag::kNone) return;
    const float w = static_cast<float>(viewer_->image_width());
    const float h = static_cast<float>(viewer_->image_height());
    float ix = 0.0f, iy = 0.0f;
    viewer_->to_image(x, y, &ix, &iy);
    ix = std::clamp(ix, 0.0f, w);
    iy = std::clamp(iy, 0.0f, h);

    if (drag_ == Drag::kDraw) {
        const float ax = std::clamp(grab_x_, 0.0f, w), ay = std::clamp(grab_y_, 0.0f, h);
        if (!drag_id_) {
            float sx = 0.0f, sy = 0.0f;
            viewer_->from_image(ax, ay, &sx, &sy);
            if (std::fabs(x - sx) < 4.0f && std::fabs(y - sy) < 4.0f) return;
            if (on_edit_begin) on_edit_begin();
            edited_ = true;
        }
        const float x0 = std::min(ax, ix), x1 = std::max(ax, ix);
        const float y0 = std::min(ay, iy), y1 = std::max(ay, iy);
        std::vector<fixes::Point> corners = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
        if (!drag_id_) {
            drag_id_ = fixes_->add_light(frame_, corners);
            select(drag_id_);
        } else {
            fixes_->find_light(drag_id_)->set(frame_, corners);
        }
    } else {
        fixes::LightCrop* crop = fixes_->find_light(drag_id_);
        if (!crop) return;
        if (!edited_) {
            if (on_edit_begin) on_edit_begin();
            edited_ = true;
        }
        std::vector<fixes::Point> corners = grabbed_;
        if (drag_ == Drag::kCorner) {
            corners[static_cast<std::size_t>(corner_)] = {ix, iy};
        } else {
            // Moved whole, and stopped at the frame's edge.
            float lo_x = w, hi_x = 0, lo_y = h, hi_y = 0;
            for (const fixes::Point& p : corners) {
                lo_x = std::min(lo_x, p.x);
                hi_x = std::max(hi_x, p.x);
                lo_y = std::min(lo_y, p.y);
                hi_y = std::max(hi_y, p.y);
            }
            const float dx = std::clamp(ix - grab_x_, -lo_x, w - hi_x);
            const float dy = std::clamp(iy - grab_y_, -lo_y, h - hi_y);
            for (fixes::Point& p : corners) {
                p.x += dx;
                p.y += dy;
            }
        }
        crop->set(frame_, std::move(corners));
    }
    if (on_live_change) on_live_change();
}

void LightTool::release() {
    if (drag_ == Drag::kPan) viewer_->end_pan();
    if (drag_ == Drag::kDraw && drag_id_) {
        // Too small to be a light is not kept.
        const std::vector<fixes::Point> c = fixes_->find_light(drag_id_)->at(frame_);
        if (std::fabs(c[2].x - c[0].x) < 16.0f || std::fabs(c[2].y - c[0].y) < 16.0f) {
            fixes_->remove_light(drag_id_);
            select(0);
        }
    }
    drag_ = Drag::kNone;
    if (edited_ && on_changed) on_changed();
    edited_ = false;
}

bool LightTool::handle_key(SDL_Keycode key, SDL_Keymod mods) {
    if (!active()) return false;
    fixes::LightCrop* crop = fixes_->find_light(selected_);
    switch (key) {
        case SDLK_ESCAPE:
            if (!selected_) return false;
            select(0);
            return true;
        case SDLK_K:
            if (!crop) return false;
            if (!crop->keyed_at(frame_)) {
                if (on_edit_begin) on_edit_begin();
                crop->set(frame_, crop->at(frame_));
                if (on_changed) on_changed();
            }
            return true;
        case SDLK_BACKSPACE:
        case SDLK_DELETE: {
            if (!crop) return false;
            if (on_edit_begin) on_edit_begin();
            const bool whole = (mods & SDL_KMOD_SHIFT) != 0 || !crop->keyed_at(frame_) ||
                               crop->keys.size() == 1;
            if (whole) {
                fixes_->remove_light(selected_);
                select(0);
            } else {
                crop->remove_key(frame_);
            }
            if (on_changed) on_changed();
            return true;
        }
        default:
            return false;
    }
}

void LightTool::paint_self(gpu::DrawList& draw, const ui::PaintContext& ctx) {
    (void)ctx;
    for (const fixes::LightCrop& crop : fixes_->lights) {
        std::vector<float> xy;
        for (const fixes::Point& p : crop.at(frame_)) {
            float sx = 0.0f, sy = 0.0f;
            viewer_->from_image(p.x, p.y, &sx, &sy);
            xy.push_back(sx);
            xy.push_back(sy);
        }
        const bool selected = crop.id == selected_;
        const gpu::Color color = selected ? theme::kWhite : theme::kHover;
        stroke_polyline(draw, xy, true, selected ? 2.0f : 1.5f, color);
        // Its top edge doubled, so which way up the light map comes out is
        // never in doubt.
        if (selected) {
            const float dx = xy[3] - xy[1], dy = xy[2] - xy[0];
            const float len = std::max(1.0f, std::hypot(dx, dy));
            const float ox = dx / len * 4.0f, oy = -dy / len * 4.0f;
            stroke_polyline(draw, {xy[0] + ox, xy[1] + oy, xy[2] + ox, xy[3] + oy}, false, 1.0f, color);
            const bool keyed = crop.keyed_at(frame_);
            for (int k = 0; k < 4; ++k) {
                gpu::RectStyle style;
                style.fill = keyed ? color : theme::kClear;
                style.border = color;
                style.border_width = 1.5f;
                style.radius = 1.5f;
                draw.add_rect(gpu::Rect{xy[k * 2] - kHandle * 0.5f, xy[k * 2 + 1] - kHandle * 0.5f, kHandle, kHandle},
                              style);
            }
        }
    }
}

}  // namespace ps::editor

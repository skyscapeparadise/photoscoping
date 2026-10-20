#include "editor/rematte_tool.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "editor/lines.hpp"
#include "editor/theme.hpp"

namespace ps::editor {

namespace {

constexpr float kReach = 8.0f;   // logical pixels either side of an edge
constexpr float kHandle = 7.0f;

// A box's corners, smaller first.
void ordered(const std::vector<fixes::Point>& p, fixes::Point out[2]) {
    out[0] = {std::min(p[0].x, p[1].x), std::min(p[0].y, p[1].y)};
    out[1] = {std::max(p[0].x, p[1].x), std::max(p[0].y, p[1].y)};
}

}  // namespace

void RematteTool::attach(Viewer* viewer, fixes::Fixes* fixes) {
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

void RematteTool::set_active(bool active) {
    visible = active;
    if (!active) drag_ = Drag::kNone;
}

std::string RematteTool::heading() const {
    const fixes::Rematte* box = fixes_->find_rematte(selected_);
    if (!box) return "re-matte";
    const fixes::PixelRect r =
        fixes::rematte_rect(*box, frame_, viewer_->image_width(), viewer_->image_height());
    if (r.width() < fixes::kMinRematte || r.height() < fixes::kMinRematte) return "re-matte";
    const fixes::RematteCrop c =
        fixes::rematte_crop(r, viewer_->image_width(), viewer_->image_height());
    char text[48];
    std::snprintf(text, sizeof text, "re-matte at %.1f\xC3\x97", c.detail);
    return text;
}

void RematteTool::select(int id) {
    if (selected_ == id) return;
    selected_ = id;
    if (on_heading_changed) on_heading_changed();
}

int RematteTool::hit_box(float x, float y, int* sides) const {
    auto test = [&](const fixes::Rematte& box) {
        if (!box.live_at(frame_) && box.id != selected_) return false;
        fixes::Point c[2];
        ordered(box.at(frame_), c);
        float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
        viewer_->from_image(c[0].x, c[0].y, &x0, &y0);
        viewer_->from_image(c[1].x, c[1].y, &x1, &y1);
        if (x < x0 - kReach || x > x1 + kReach || y < y0 - kReach || y > y1 + kReach) return false;
        int s = 0;
        if (std::fabs(x - x0) <= kReach) s |= kLeft;
        else if (std::fabs(x - x1) <= kReach) s |= kRight;
        if (std::fabs(y - y0) <= kReach) s |= kTop;
        else if (std::fabs(y - y1) <= kReach) s |= kBottom;
        *sides = s;
        return true;
    };
    if (const fixes::Rematte* sel = fixes_->find_rematte(selected_); sel && test(*sel)) return sel->id;
    for (auto it = fixes_->rematte.rbegin(); it != fixes_->rematte.rend(); ++it)
        if (test(*it)) return it->id;
    return 0;
}

void RematteTool::press(float x, float y) {
    edited_ = false;
    if (viewer_->space_held()) {
        drag_ = Drag::kPan;
        viewer_->begin_pan(x, y);
        return;
    }
    viewer_->to_image(x, y, &grab_x_, &grab_y_);
    int sides = 0;
    const int id = hit_box(x, y, &sides);
    if (id) {
        select(id);
        drag_ = sides ? Drag::kResize : Drag::kMove;
        drag_id_ = id;
        sides_ = sides;
        ordered(fixes_->find_rematte(id)->at(frame_), grabbed_);
        return;
    }
    // A new box -- made once the drag has some size, so a click on nothing
    // only lets go of the selection.
    drag_ = Drag::kDraw;
    drag_id_ = 0;
    select(0);
}

void RematteTool::drag(float x, float y) {
    if (drag_ == Drag::kPan) {
        viewer_->pan_to(x, y);
        return;
    }
    if (drag_ == Drag::kNone) return;
    float ix = 0.0f, iy = 0.0f;
    viewer_->to_image(x, y, &ix, &iy);
    const float w = static_cast<float>(viewer_->image_width());
    const float h = static_cast<float>(viewer_->image_height());
    ix = std::clamp(ix, 0.0f, w);
    iy = std::clamp(iy, 0.0f, h);

    if (drag_ == Drag::kDraw) {
        const fixes::Point a{std::clamp(grab_x_, 0.0f, w), std::clamp(grab_y_, 0.0f, h)};
        if (!drag_id_) {
            // Some logical pixels of drag before it is a box.
            float ax = 0.0f, ay = 0.0f;
            viewer_->from_image(a.x, a.y, &ax, &ay);
            if (std::fabs(x - ax) < 4.0f && std::fabs(y - ay) < 4.0f) return;
            if (on_edit_begin) on_edit_begin();
            edited_ = true;
            drag_id_ = fixes_->add_rematte(frame_, a, {ix, iy});
            select(drag_id_);
        }
        fixes::Point c[2];
        ordered({a, {ix, iy}}, c);
        fixes_->find_rematte(drag_id_)->set(frame_, {c[0], c[1]});
    } else {
        fixes::Rematte* box = fixes_->find_rematte(drag_id_);
        if (!box) return;
        if (!edited_) {
            if (on_edit_begin) on_edit_begin();
            edited_ = true;
        }
        fixes::Point c[2] = {grabbed_[0], grabbed_[1]};
        const float dx = ix - grab_x_, dy = iy - grab_y_;
        if (drag_ == Drag::kMove) {
            // Moved whole, and stopped at the frame's edge without changing
            // size.
            const float mx = std::clamp(dx, -c[0].x, w - c[1].x);
            const float my = std::clamp(dy, -c[0].y, h - c[1].y);
            for (fixes::Point& p : c) {
                p.x += mx;
                p.y += my;
            }
        } else {
            if (sides_ & kLeft) c[0].x = std::clamp(c[0].x + dx, 0.0f, w);
            if (sides_ & kRight) c[1].x = std::clamp(c[1].x + dx, 0.0f, w);
            if (sides_ & kTop) c[0].y = std::clamp(c[0].y + dy, 0.0f, h);
            if (sides_ & kBottom) c[1].y = std::clamp(c[1].y + dy, 0.0f, h);
        }
        // Dragged past the opposite side, the box turns inside out; kept
        // smaller corner first, as every key is.
        fixes::Point o[2];
        ordered({c[0], c[1]}, o);
        box->set(frame_, {o[0], o[1]});
    }
    if (on_heading_changed) on_heading_changed();
    if (on_live_change) on_live_change();
}

void RematteTool::release() {
    if (drag_ == Drag::kPan) viewer_->end_pan();
    if (drag_ == Drag::kDraw && drag_id_) {
        // Too small to re-matte is not kept.
        const fixes::Rematte* box = fixes_->find_rematte(drag_id_);
        const fixes::PixelRect r =
            fixes::rematte_rect(*box, frame_, viewer_->image_width(), viewer_->image_height());
        if (r.width() < fixes::kMinRematte || r.height() < fixes::kMinRematte) {
            fixes_->remove_rematte(drag_id_);
            select(0);
        }
    }
    drag_ = Drag::kNone;
    finish(edited_);
}

void RematteTool::finish(bool edited) {
    if (edited && on_changed) on_changed();
    edited_ = false;
}

bool RematteTool::handle_key(SDL_Keycode key, SDL_Keymod mods) {
    if (!active()) return false;
    fixes::Rematte* box = fixes_->find_rematte(selected_);
    switch (key) {
        case SDLK_ESCAPE:
            if (!selected_) return false;
            select(0);
            return true;
        case SDLK_K:
            if (!box) return false;
            if (!box->keyed_at(frame_)) {
                if (on_edit_begin) on_edit_begin();
                box->set(frame_, box->at(frame_));
                finish(true);
            }
            return true;
        case SDLK_BACKSPACE:
        case SDLK_DELETE: {
            if (!box) return false;
            if (on_edit_begin) on_edit_begin();
            const bool whole = (mods & SDL_KMOD_SHIFT) != 0 || !box->keyed_at(frame_) ||
                               box->keys.size() == 1;
            if (whole) {
                fixes_->remove_rematte(selected_);
                select(0);
            } else {
                box->remove_key(frame_);
            }
            finish(true);
            if (on_heading_changed) on_heading_changed();
            return true;
        }
        default:
            return false;
    }
}

void RematteTool::paint_self(gpu::DrawList& draw, const ui::PaintContext& ctx) {
    (void)ctx;
    const int iw = viewer_->image_width(), ih = viewer_->image_height();
    for (const fixes::Rematte& box : fixes_->rematte) {
        const bool selected = box.id == selected_;
        const bool live = box.live_at(frame_);
        if (!live && !selected) continue;
        fixes::Point c[2];
        ordered(box.at(frame_), c);
        float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
        viewer_->from_image(c[0].x, c[0].y, &x0, &y0);
        viewer_->from_image(c[1].x, c[1].y, &x1, &y1);

        // White where it is re-matted, lilac where it waits; faint off the
        // frames it covers, where only a K would bring it.
        const fixes::PixelRect r = fixes::rematte_rect(box, frame_, iw, ih);
        const bool made = live && is_made && is_made(frame_, r);
        gpu::Color color = made ? theme::kWhite : theme::kHover;
        if (!live) color.a = 0.35f;
        stroke_polyline(draw, {x0, y0, x1, y0, x1, y1, x0, y1}, true, selected ? 2.0f : 1.5f, color);
        if (!selected) continue;
        const bool keyed = box.keyed_at(frame_);
        for (const auto& [hx, hy] : {std::pair{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}) {
            gpu::RectStyle style;
            style.fill = keyed ? color : theme::kClear;
            style.border = color;
            style.border_width = 1.5f;
            style.radius = 1.5f;
            draw.add_rect(gpu::Rect{hx - kHandle * 0.5f, hy - kHandle * 0.5f, kHandle, kHandle}, style);
        }
    }
}

}  // namespace ps::editor

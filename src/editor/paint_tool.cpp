#include "editor/paint_tool.hpp"

#include <algorithm>
#include <cmath>

#include "editor/lines.hpp"
#include "editor/theme.hpp"

namespace ps::editor {

namespace {

constexpr float kMinRadius = 1.0f;
constexpr float kMaxRadius = 2000.0f;
constexpr float kReach = 6.0f;  // logical pixels past a stroke's edge

float distance2(float x, float y, float ax, float ay, float bx, float by) {
    const float dx = bx - ax, dy = by - ay;
    const float len2 = dx * dx + dy * dy;
    float t = len2 > 0.0f ? ((x - ax) * dx + (y - ay) * dy) / len2 : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    const float px = ax + t * dx - x, py = ay + t * dy - y;
    return px * px + py * py;
}

gpu::Color mode_colour(fixes::PaintMode mode, float alpha) {
    gpu::Color c = mode == fixes::PaintMode::kOut ? theme::kHover : theme::kWhite;
    c.a = alpha;
    return c;
}

std::vector<float> circle(float cx, float cy, float r) {
    constexpr int kSegments = 48;
    std::vector<float> xy;
    for (int i = 0; i < kSegments; ++i) {
        const float t = 6.2831853f * static_cast<float>(i) / kSegments;
        xy.push_back(cx + r * std::cos(t));
        xy.push_back(cy + r * std::sin(t));
    }
    return xy;
}

}  // namespace

void PaintTool::attach(Viewer* viewer, fixes::Fixes* fixes) {
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
    on_pointer_moved = [this](float x, float y) {
        pointer_x_ = x;
        pointer_y_ = y;
    };
    on_hover_changed = [this](bool inside) {
        if (!inside) pointer_x_ = pointer_y_ = -1.0f;
    };
}

void PaintTool::set_active(bool active) {
    visible = active;
    if (!active) drag_ = Drag::kNone;
}

std::string PaintTool::heading() const {
    return mode_ == fixes::PaintMode::kOut ? "paint out" : "paint in";
}

int PaintTool::hit_stroke(float x, float y) const {
    // On screen, so the reach is the same at any zoom; the last painted is
    // on top, so it is asked first.
    for (auto it = fixes_->strokes.rbegin(); it != fixes_->strokes.rend(); ++it) {
        if (!it->live_at(frame_)) continue;
        const std::vector<fixes::Point> pts = it->at(frame_);
        const float reach = it->radius * viewer_->scale() + kReach;
        for (std::size_t i = 0; i < pts.size(); ++i) {
            const fixes::Point& a = pts[i];
            const fixes::Point& b = pts[std::min(i + 1, pts.size() - 1)];
            float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f;
            viewer_->from_image(a.x, a.y, &ax, &ay);
            viewer_->from_image(b.x, b.y, &bx, &by);
            if (distance2(x, y, ax, ay, bx, by) <= reach * reach) return it->id;
        }
    }
    return 0;
}

void PaintTool::press(float x, float y) {
    edited_ = false;
    if (viewer_->space_held()) {
        drag_ = Drag::kPan;
        viewer_->begin_pan(x, y);
        return;
    }
    float ix = 0.0f, iy = 0.0f;
    viewer_->to_image(x, y, &ix, &iy);
    const SDL_Keymod mods = SDL_GetModState();

    if ((mods & SDL_KMOD_GUI) != 0) {
        const int id = hit_stroke(x, y);
        if (!id) return;
        drag_ = Drag::kMove;
        drag_id_ = selected_ = id;
        grab_x_ = ix;
        grab_y_ = iy;
        grabbed_ = fixes_->find_stroke(id)->at(frame_);
        return;
    }

    // A new stroke, made now so the matte answers as it is painted.
    fixes::PaintMode mode = mode_;
    if ((mods & SDL_KMOD_ALT) != 0)
        mode = mode == fixes::PaintMode::kOut ? fixes::PaintMode::kIn : fixes::PaintMode::kOut;
    if (on_edit_begin) on_edit_begin();
    edited_ = true;
    stroke_ = {{ix, iy}};
    drag_ = Drag::kPaint;
    drag_id_ = selected_ = fixes_->add_stroke(frame_, mode, radius_, stroke_);
    if (on_live_change) on_live_change();
}

void PaintTool::drag(float x, float y) {
    pointer_x_ = x;
    pointer_y_ = y;
    if (drag_ == Drag::kPan) {
        viewer_->pan_to(x, y);
        return;
    }
    fixes::Stroke* s = fixes_->find_stroke(drag_id_);
    if (!s || drag_ == Drag::kNone) return;
    float ix = 0.0f, iy = 0.0f;
    viewer_->to_image(x, y, &ix, &iy);
    if (drag_ == Drag::kPaint) {
        // A point every quarter of the brush is smooth enough, and keeps a
        // long stroke from being thousands of them.
        const fixes::Point& last = stroke_.back();
        const float step = std::max(1.0f, s->radius * 0.25f);
        if ((ix - last.x) * (ix - last.x) + (iy - last.y) * (iy - last.y) < step * step) return;
        stroke_.push_back({ix, iy});
        s->set(frame_, stroke_);
    } else {
        if (!edited_) {
            if (on_edit_begin) on_edit_begin();
            edited_ = true;
        }
        std::vector<fixes::Point> pts = grabbed_;
        for (fixes::Point& p : pts) {
            p.x += ix - grab_x_;
            p.y += iy - grab_y_;
        }
        s->set(frame_, std::move(pts));
    }
    if (on_live_change) on_live_change();
}

void PaintTool::release() {
    if (drag_ == Drag::kPan) viewer_->end_pan();
    drag_ = Drag::kNone;
    finish(edited_);
}

void PaintTool::finish(bool edited) {
    if (edited && on_changed) on_changed();
    edited_ = false;
}

bool PaintTool::handle_key(SDL_Keycode key, SDL_Keymod mods) {
    if (!active()) return false;
    switch (key) {
        case SDLK_X:
            mode_ = mode_ == fixes::PaintMode::kOut ? fixes::PaintMode::kIn : fixes::PaintMode::kOut;
            if (on_brush_changed) on_brush_changed();
            return true;
        case SDLK_LEFTBRACKET:
        case SDLK_RIGHTBRACKET:
            radius_ = std::clamp(key == SDLK_LEFTBRACKET ? radius_ / 1.25f : radius_ * 1.25f,
                                 kMinRadius, kMaxRadius);
            if (on_brush_changed) on_brush_changed();
            return true;
        case SDLK_K: {
            fixes::Stroke* s = fixes_->find_stroke(selected_);
            if (!s || s->keyed_at(frame_)) return s != nullptr;
            if (on_edit_begin) on_edit_begin();
            s->set(frame_, s->at(frame_));
            finish(true);
            return true;
        }
        case SDLK_BACKSPACE:
        case SDLK_DELETE: {
            fixes::Stroke* s = fixes_->find_stroke(selected_);
            if (!s) return false;
            if (on_edit_begin) on_edit_begin();
            const bool whole = (mods & SDL_KMOD_SHIFT) != 0 || !s->keyed_at(frame_) ||
                               s->keys.size() == 1;
            if (whole) {
                fixes_->remove_stroke(selected_);
                selected_ = 0;
            } else {
                s->remove_key(frame_);
            }
            finish(true);
            return true;
        }
        default:
            return false;
    }
}

void PaintTool::paint_self(gpu::DrawList& draw, const ui::PaintContext& ctx) {
    (void)ctx;
    const float scale = viewer_->scale();
    for (const fixes::Stroke& s : fixes_->strokes) {
        if (!s.live_at(frame_)) continue;
        const std::vector<fixes::Point> pts = s.at(frame_);
        std::vector<float> xy;
        for (const fixes::Point& p : pts) {
            float x = 0.0f, y = 0.0f;
            viewer_->from_image(p.x, p.y, &x, &y);
            xy.push_back(x);
            xy.push_back(y);
        }
        // Its centre line, and the brush's outline at either end -- enough to
        // find it and see its reach; the matte shows what it does.
        const bool selected = s.id == selected_;
        const gpu::Color line = mode_colour(s.mode, selected ? 0.9f : 0.45f);
        const float r = s.radius * scale;
        if (xy.size() > 2) stroke_polyline(draw, xy, false, selected ? 1.5f : 1.0f, line);
        stroke_polyline(draw, circle(xy[0], xy[1], r), true, 1.0f, line);
        if (xy.size() > 2)
            stroke_polyline(draw, circle(xy[xy.size() - 2], xy.back(), r), true, 1.0f, line);
    }
    // The brush, under the pointer, in the colour of the way it will paint.
    if (pointer_x_ >= 0.0f && drag_ != Drag::kMove) {
        const bool other = (SDL_GetModState() & SDL_KMOD_ALT) != 0;
        const fixes::PaintMode mode =
            other ? (mode_ == fixes::PaintMode::kOut ? fixes::PaintMode::kIn : fixes::PaintMode::kOut)
                  : mode_;
        stroke_polyline(draw, circle(pointer_x_, pointer_y_, radius_ * scale), true, 1.5f,
                        mode_colour(mode, 0.95f));
    }
}

}  // namespace ps::editor

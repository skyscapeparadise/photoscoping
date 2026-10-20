#include "editor/garbage_tool.hpp"

#include <algorithm>
#include <cmath>

#include "editor/lines.hpp"
#include "editor/theme.hpp"

namespace ps::editor {

namespace {

// How near, in logical pixels, a press has to land to take a point.
constexpr float kReach = 8.0f;
constexpr float kHandle = 7.0f;

// Squared distance from (x, y) to the segment a-b.
float distance2(float x, float y, float ax, float ay, float bx, float by) {
    const float dx = bx - ax, dy = by - ay;
    const float len2 = dx * dx + dy * dy;
    float t = len2 > 0.0f ? ((x - ax) * dx + (y - ay) * dy) / len2 : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    const float px = ax + t * dx - x, py = ay + t * dy - y;
    return px * px + py * py;
}

}  // namespace

void GarbageTool::attach(Viewer* viewer, fixes::Fixes* fixes) {
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
    on_double_click = [this] {
        if (drawing_.size() >= 4) {
            // The double-click's first click added a point where the second
            // landed; it is the same point, and closing makes it the last.
            drawing_.pop_back();
            close_shape();
        }
    };
    on_pointer_moved = [this](float x, float y) {
        pointer_x_ = x;
        pointer_y_ = y;
    };
}

void GarbageTool::set_active(bool active) {
    visible = active;
    if (!active) {
        drawing_.clear();
        drag_ = Drag::kNone;
    }
}

void GarbageTool::begin_edit() {
    if (edited_) return;
    edited_ = true;
    if (on_edit_begin) on_edit_begin();
}

bool GarbageTool::hit_point(float x, float y, int* id, int* index) const {
    float best = kReach * kReach;
    bool found = false;
    for (const fixes::Garbage& g : fixes_->garbage) {
        const std::vector<fixes::Point> pts = g.at(frame_);
        for (std::size_t i = 0; i < pts.size(); ++i) {
            float sx = 0.0f, sy = 0.0f;
            viewer_->from_image(pts[i].x, pts[i].y, &sx, &sy);
            const float d = (sx - x) * (sx - x) + (sy - y) * (sy - y);
            if (d <= best) {
                best = d;
                *id = g.id;
                *index = static_cast<int>(i);
                found = true;
            }
        }
    }
    return found;
}

int GarbageTool::hit_edge(float x, float y) const {
    // Measured on screen, so the reach is the same at any zoom. The last
    // drawn is on top, so it is asked first.
    for (auto it = fixes_->garbage.rbegin(); it != fixes_->garbage.rend(); ++it) {
        const std::vector<fixes::Point> pts = it->at(frame_);
        for (std::size_t i = 0, n = pts.size(); i < n; ++i) {
            float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f;
            viewer_->from_image(pts[i].x, pts[i].y, &ax, &ay);
            viewer_->from_image(pts[(i + 1) % n].x, pts[(i + 1) % n].y, &bx, &by);
            if (distance2(x, y, ax, ay, bx, by) <= kReach * kReach) return it->id;
        }
    }
    return 0;
}

void GarbageTool::press(float x, float y) {
    edited_ = false;
    if (viewer_->space_held()) {
        drag_ = Drag::kPan;
        viewer_->begin_pan(x, y);
        return;
    }
    float ix = 0.0f, iy = 0.0f;
    viewer_->to_image(x, y, &ix, &iy);

    if (!drawing_.empty()) {
        float fx = 0.0f, fy = 0.0f;
        viewer_->from_image(drawing_.front().x, drawing_.front().y, &fx, &fy);
        const bool on_first = (fx - x) * (fx - x) + (fy - y) * (fy - y) <= kReach * kReach;
        if (on_first && drawing_.size() >= 3) close_shape();
        else drawing_.push_back({ix, iy});
        return;
    }

    int id = 0, index = 0;
    if (hit_point(x, y, &id, &index)) {
        drag_ = Drag::kVertex;
        drag_id_ = selected_ = id;
        drag_index_ = index;
        return;
    }
    if (const int id_edge = hit_edge(x, y)) {
        drag_ = Drag::kShape;
        drag_id_ = selected_ = id_edge;
        grab_x_ = ix;
        grab_y_ = iy;
        grabbed_ = fixes_->find(id_edge)->at(frame_);
        return;
    }
    // Nothing here: the start of a new shape.
    selected_ = 0;
    drawing_.push_back({ix, iy});
}

void GarbageTool::drag(float x, float y) {
    pointer_x_ = x;
    pointer_y_ = y;
    if (drag_ == Drag::kPan) {
        viewer_->pan_to(x, y);
        return;
    }
    fixes::Garbage* g = fixes_->find(drag_id_);
    if (!g || drag_ == Drag::kNone) return;
    float ix = 0.0f, iy = 0.0f;
    viewer_->to_image(x, y, &ix, &iy);
    begin_edit();
    std::vector<fixes::Point> pts;
    if (drag_ == Drag::kVertex) {
        pts = g->at(frame_);
        pts[drag_index_] = {ix, iy};
    } else {
        pts = grabbed_;
        for (fixes::Point& p : pts) {
            p.x += ix - grab_x_;
            p.y += iy - grab_y_;
        }
    }
    g->set(frame_, std::move(pts));
    if (on_live_change) on_live_change();
}

void GarbageTool::release() {
    if (drag_ == Drag::kPan) viewer_->end_pan();
    drag_ = Drag::kNone;
    if (edited_ && on_changed) on_changed();
    edited_ = false;
}

void GarbageTool::close_shape() {
    if (drawing_.size() < 3) return;
    edited_ = false;
    begin_edit();
    selected_ = fixes_->add_garbage(frame_, std::move(drawing_));
    drawing_.clear();
    if (on_changed) on_changed();
    edited_ = false;
}

bool GarbageTool::handle_key(SDL_Keycode key, SDL_Keymod mods) {
    if (!active()) return false;
    switch (key) {
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            if (drawing_.empty()) return false;
            close_shape();
            return true;
        case SDLK_ESCAPE:
            if (drawing_.empty()) return false;
            drawing_.clear();
            return true;
        case SDLK_BACKSPACE:
        case SDLK_DELETE: {
            if (!drawing_.empty()) {
                drawing_.pop_back();
                return true;
            }
            fixes::Garbage* g = fixes_->find(selected_);
            if (!g) return false;
            edited_ = false;
            begin_edit();
            // Its key here, unless that is all there is of it -- or shift,
            // which means the whole shape.
            const bool whole = (mods & SDL_KMOD_SHIFT) != 0 || !g->keyed_at(frame_) ||
                               g->keys.size() == 1;
            if (whole) {
                fixes_->remove_garbage(selected_);
                selected_ = 0;
            } else {
                g->remove_key(frame_);
            }
            if (on_changed) on_changed();
            edited_ = false;
            return true;
        }
        default:
            return false;
    }
}

void GarbageTool::paint_self(gpu::DrawList& draw, const ui::PaintContext& ctx) {
    (void)ctx;
    auto screen = [&](const std::vector<fixes::Point>& pts) {
        std::vector<float> xy;
        xy.reserve(pts.size() * 2);
        for (const fixes::Point& p : pts) {
            float x = 0.0f, y = 0.0f;
            viewer_->from_image(p.x, p.y, &x, &y);
            xy.push_back(x);
            xy.push_back(y);
        }
        return xy;
    };
    auto handle = [&](float x, float y, bool keyed) {
        // Filled on a frame with a key, hollow where the shape is in between.
        gpu::RectStyle style;
        style.fill = keyed ? theme::kWhite : theme::kClear;
        style.border = theme::kWhite;
        style.border_width = 1.5f;
        style.radius = 1.5f;
        draw.add_rect(gpu::Rect{x - kHandle * 0.5f, y - kHandle * 0.5f, kHandle, kHandle}, style);
    };

    for (const fixes::Garbage& g : fixes_->garbage) {
        const std::vector<float> xy = screen(g.at(frame_));
        const bool selected = g.id == selected_;
        stroke_polyline(draw, xy, true, selected ? 2.0f : 1.5f,
                        selected ? theme::kWhite : theme::kHover);
        const bool keyed = g.keyed_at(frame_);
        for (std::size_t i = 0; i + 1 < xy.size(); i += 2) handle(xy[i], xy[i + 1], keyed);
    }

    if (!drawing_.empty()) {
        std::vector<float> xy = screen(drawing_);
        xy.push_back(pointer_x_);
        xy.push_back(pointer_y_);
        stroke_polyline(draw, xy, false, 1.5f, theme::kWhite);
        for (std::size_t i = 0; i + 3 < xy.size(); i += 2) handle(xy[i], xy[i + 1], true);
    }
}

}  // namespace ps::editor

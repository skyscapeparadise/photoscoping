#include "ui/node.hpp"
#include <cmath>

#include <algorithm>

namespace ps::ui {

namespace {

float margin_or(float specific, float general) {
    return specific != 0.0f ? specific : general;
}

}  // namespace

void Node::layout(float parent_w, float parent_h, float parent_abs_x,
                  float parent_abs_y) {
    const Anchors& a = anchors;

    if (a.fill) {
        const float l = margin_or(a.left_margin, a.margins);
        const float r = margin_or(a.right_margin, a.margins);
        const float t = margin_or(a.top_margin, a.margins);
        const float b = margin_or(a.bottom_margin, a.margins);
        x = l;
        y = t;
        width = std::max(0.0f, parent_w - l - r);
        height = std::max(0.0f, parent_h - t - b);
    } else if (a.center_in) {
        x = (parent_w - width) * 0.5f;
        y = (parent_h - height) * 0.5f;
    } else {
        // Edge anchors. Anchoring both sides derives the size; anchoring one
        // keeps the explicit size and positions against that edge.
        if (a.left && a.right) {
            const float l = margin_or(a.left_margin, a.margins);
            const float r = margin_or(a.right_margin, a.margins);
            x = l;
            width = std::max(0.0f, parent_w - l - r);
        } else if (a.left) {
            x = margin_or(a.left_margin, a.margins);
        } else if (a.right) {
            x = parent_w - margin_or(a.right_margin, a.margins) - width;
        } else if (a.h_center) {
            x = (parent_w - width) * 0.5f + a.h_center_offset;
        }

        if (a.top && a.bottom) {
            const float t = margin_or(a.top_margin, a.margins);
            const float b = margin_or(a.bottom_margin, a.margins);
            y = t;
            height = std::max(0.0f, parent_h - t - b);
        } else if (a.top) {
            y = margin_or(a.top_margin, a.margins);
        } else if (a.bottom) {
            y = parent_h - margin_or(a.bottom_margin, a.margins) - height;
        } else if (a.v_center) {
            y = (parent_h - height) * 0.5f + a.v_center_offset;
        }
    }

    abs_x_ = parent_abs_x + x;
    abs_y_ = parent_abs_y + y;

    layout_self();

    for (auto& child : children_) {
        child->layout(width, height, abs_x_, abs_y_);
    }
}

bool Node::tick(float dt) {
    bool animating = opacity.tick(dt);
    animating |= tick_self(dt);
    for (auto& child : children_) {
        animating |= child->tick(dt);
    }
    return animating;
}

void Node::paint(gpu::DrawList& draw, const PaintContext& ctx) {
    if (!visible || opacity.value() <= 0.0f) return;

    PaintContext self = ctx;
    self.origin_x = ctx.origin_x + x;
    self.origin_y = ctx.origin_y + y;
    self.opacity = ctx.opacity * opacity.value();

    paint_self(draw, self);

    // A clipping node bounds its children, not itself: its own background is
    // drawn by paint_self above, already inside those bounds.
    if (clip) {
        draw.push_clip(gpu::Rect{self.origin_x, self.origin_y, width, height},
                       clip_radius);
    }
    for (auto& child : children_) {
        child->paint(draw, self);
    }
    if (clip) draw.pop_clip();
}

Node* Node::hit_test(float px, float py, float, float) {
    if (!visible || opacity.value() <= 0.0f) return nullptr;

    const float ox = abs_x_;
    const float oy = abs_y_;

    // Children first, last-drawn first: the topmost node wins, matching what
    // the user sees. A parent that clips also rejects points outside itself,
    // so a clipped-away child is not secretly clickable.
    const bool inside = px >= ox && px < ox + width && py >= oy && py < oy + height;
    if (clip && !inside) return nullptr;

    for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
        if (Node* hit = (*it)->hit_test(px, py)) return hit;
    }

    return (inside && accepts_input()) ? this : nullptr;
}

namespace {
// There is one router, so one pending request is enough. Cleared as soon as it
// is honoured, and cleared again by node_destroyed, so a node that goes away
// before the next update cannot be focused after it is gone.
Node* g_focus_request = nullptr;

// Every live router, so a destroyed node can be cleared from all of them. A
// vector rather than a single pointer because the tests build their own.
std::vector<InputRouter*>& routers() {
    static std::vector<InputRouter*> list;
    return list;
}
}  // namespace

InputRouter::InputRouter() { routers().push_back(this); }

InputRouter::~InputRouter() {
    auto& list = routers();
    list.erase(std::remove(list.begin(), list.end(), this), list.end());
}

Node::~Node() { InputRouter::node_destroyed(this); }

void InputRouter::node_destroyed(Node* node) {
    if (!node) return;
    for (InputRouter* r : routers()) {
        if (r->hover_ == node) r->hover_ = nullptr;
        if (r->pressed_node_ == node) r->pressed_node_ = nullptr;
        if (r->right_pressed_node_ == node) r->right_pressed_node_ = nullptr;
        if (r->middle_pressed_node_ == node) r->middle_pressed_node_ = nullptr;
        if (r->last_click_node_ == node) r->last_click_node_ = nullptr;
        if (r->focus_ == node) r->focus_ = nullptr;
    }
    if (g_focus_request == node) g_focus_request = nullptr;
}

void Node::raise() {
    if (!parent_) return;
    auto& siblings = parent_->children_;
    for (std::size_t i = 0; i + 1 < siblings.size(); ++i) {
        if (siblings[i].get() != this) continue;
        std::unique_ptr<Node> self = std::move(siblings[i]);
        siblings.erase(siblings.begin() + static_cast<std::ptrdiff_t>(i));
        siblings.push_back(std::move(self));
        return;
    }
}

void InputRouter::set_pointer(float x, float y) {
    px_ = x;
    py_ = y;
    inside_ = true;
}

void InputRouter::pointer_left() {
    inside_ = false;
    px_ = py_ = -1.0f;
}

void InputRouter::press() { press_pending_ = true; pressed_ = true; }

void InputRouter::release() { pressed_ = false; }

void InputRouter::press_right() {
    // Routed immediately rather than deferred to update(): the hover target is
    // already current, and a hold has to start on the frame the button went
    // down or its progress is short by one.
    // Up the parent chain until one takes it. A row whose right button
    // means "delete this" is covered by children that only care about the
    // left, and without this the press would stop at whichever of them
    // happened to be under the pointer.
    right_pressed_node_ = nullptr;
    for (Node* n = hover_; n; n = n->parent()) {
        if (!n->on_right_press) continue;
        right_pressed_node_ = n;
        n->on_right_press(px_, py_);
        break;
    }
}

void InputRouter::press_middle() {
    middle_pressed_node_ = hover_;
    if (middle_pressed_node_ && middle_pressed_node_->on_middle_press)
        middle_pressed_node_->on_middle_press(px_, py_);
}

void InputRouter::release_middle() {
    if (middle_pressed_node_ && middle_pressed_node_->on_middle_release)
        middle_pressed_node_->on_middle_release();
    middle_pressed_node_ = nullptr;
}

void InputRouter::release_right() {
    // Delivered to the node the press started on, even if the pointer has
    // since left it -- otherwise a hold that drifts never gets cancelled.
    if (right_pressed_node_ && right_pressed_node_->on_right_release)
        right_pressed_node_->on_right_release();
    right_pressed_node_ = nullptr;
}

namespace {
// How far a pointer has to travel before a press becomes a drag.
constexpr float kStealDistance = 10.0f;
}  // namespace

void InputRouter::request_focus(Node* node) { g_focus_request = node; }

void InputRouter::set_focus(Node* node) {
    if (focus_ == node) return;
    if (focus_) {
        focus_->focused_ = false;
        if (focus_->on_focus_changed) focus_->on_focus_changed(false);
    }
    focus_ = node;
    if (focus_) {
        focus_->focused_ = true;
        if (focus_->on_focus_changed) focus_->on_focus_changed(true);
    }
}

bool InputRouter::handle_key(SDL_Keycode key, SDL_Keymod mods) {
    if (focus_ && focus_->on_key) return focus_->on_key(key, mods);
    return false;
}

void InputRouter::handle_text(const char* utf8) {
    if (focus_ && focus_->on_text_input) focus_->on_text_input(utf8);
}

void InputRouter::file_dropped(const char* path, Node& root) {
    if (!inside_) return;
    for (Node* n = root.hit_test(px_, py_); n; n = n->parent()) {
        if (n->on_file_dropped) {
            n->on_file_dropped(path);
            return;
        }
    }
}

void InputRouter::wheel(float dx, float dy, Node& root) {
    if (!inside_) return;
    for (Node* n = root.hit_test(px_, py_); n; n = n->parent()) {
        if (n->on_wheel && n->on_wheel(dx, dy)) return;
    }
}

void InputRouter::update(Node& root) {
    if (g_focus_request) {
        set_focus(g_focus_request);
        g_focus_request = nullptr;
    }
    Node* target = inside_ ? root.hit_test(px_, py_) : nullptr;

    if (target != hover_) {
        if (hover_) {
            hover_->hovered_ = false;
            if (hover_->on_hover_changed) hover_->on_hover_changed(false);
        }
        hover_ = target;
        if (hover_) {
            hover_->hovered_ = true;
            if (hover_->on_hover_changed) hover_->on_hover_changed(true);
        }
    }

    // Delivered every update while the pointer is over it, which is what a
    // node drawing its own sub-targets needs to know where the pointer is.
    if (hover_ && hover_->on_pointer_moved) hover_->on_pointer_moved(px_, py_);

    if (press_pending_) {
        press_pending_ = false;
        pressed_node_ = target;
        press_x_ = px_;
        press_y_ = py_;
        drag_stolen_ = false;

        // Focus follows the press, and a press on anything that does not want
        // the keyboard clears it -- which is what makes clicking away from a
        // field commit it.
        Node* focus_target = nullptr;
        for (Node* n = target; n; n = n->parent()) {
            if (n->focusable) { focus_target = n; break; }
        }
        set_focus(focus_target);

        if (pressed_node_ && pressed_node_->on_press) {
            pressed_node_->on_press(px_, py_);
        }
    }

    // A drag that has travelled far enough is offered to whatever the press
    // landed inside that scrolls, before the node it landed on gets to carry
    // on with it. Only along the way it scrolls: a sideways drag inside a
    // list is the row's business, not the list's, and the reverse inside
    // something that scrolls sideways.
    const float travel_x = std::abs(px_ - press_x_);
    const float travel_y = std::abs(py_ - press_y_);
    if (pressed_ && !drag_stolen_ && pressed_node_ && !pressed_node_->keeps_drag &&
        std::max(travel_x, travel_y) > kStealDistance) {
        for (Node* n = pressed_node_->parent(); n; n = n->parent()) {
            if (!n->steals_drag) continue;
            const float along = n->steals_sideways ? travel_x : travel_y;
            const float across = n->steals_sideways ? travel_y : travel_x;
            if (along <= kStealDistance || along <= across) continue;
            // The row is told the gesture ended, so whatever it lit up goes
            // out. It gets no click: the release checks pressed_node_ against
            // what is under the pointer, and this is no longer that node.
            if (pressed_node_->on_release) pressed_node_->on_release();
            drag_stolen_ = true;
            pressed_node_ = n;
            // From where the finger started, not where it has reached, so the
            // content stays under it rather than jumping by the threshold.
            if (n->on_press) n->on_press(press_x_, press_y_);
            break;
        }
    }

    // Drag goes to the node the press started on, not the one currently under
    // the pointer. A slider grabbed and dragged past its own edge must keep
    // tracking, which is the whole reason this is separate from hover.
    if (pressed_ && pressed_node_ && pressed_node_->on_drag) {
        pressed_node_->on_drag(px_, py_);
    }

    // Click fires on release over the same node the press started on, which is
    // what lets a user press, drag away, and release to cancel.
    if (!pressed_ && pressed_node_) {
        if (pressed_node_->on_release) pressed_node_->on_release();
        if (pressed_node_ == target) {
            const Uint64 now = SDL_GetTicks();
            const bool is_double = (last_click_node_ == target) &&
                                   (now - last_click_ms_ <= 400);
            if (is_double && target->on_double_click) {
                target->on_double_click();
                last_click_node_ = nullptr;  // a third click starts a new pair
            } else {
                if (target->on_click) target->on_click();
                last_click_node_ = target;
                last_click_ms_ = now;
            }
        }
        pressed_node_ = nullptr;
    }
}

}  // namespace ps::ui

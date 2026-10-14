#pragma once

#include <SDL3/SDL.h>

#include <functional>
#include <string>
#include <memory>
#include <vector>

#include "gpu/draw_list.hpp"
#include "ui/animated.hpp"

namespace ps::ui {

using gpu::Color;
using gpu::Rect;

// Parent-relative anchoring, deliberately not a general constraint solver.
//
// The vocabulary is small: fill, centerIn, the four edges with margins, and
// the two centre lines with offsets. Implementing exactly that resolves in a
// single pass with no solver, no cycles, and no ordering surprises.
struct Anchors {
    bool fill = false;
    bool center_in = false;

    bool left = false, right = false, top = false, bottom = false;
    bool h_center = false, v_center = false;

    float margins = 0.0f;  // applies to any edge without a specific margin
    float left_margin = 0.0f, right_margin = 0.0f;
    float top_margin = 0.0f, bottom_margin = 0.0f;
    float h_center_offset = 0.0f, v_center_offset = 0.0f;

    static Anchors Fill(float margin = 0.0f) {
        Anchors a; a.fill = true; a.margins = margin; return a;
    }
    static Anchors CenterIn() {
        Anchors a; a.center_in = true; return a;
    }
};

class Node;

// What a node needs while painting: where it is on screen, and the inherited
// opacity, which multiplies down the tree.
struct PaintContext {
    float origin_x = 0.0f;
    float origin_y = 0.0f;
    float opacity = 1.0f;
};

// A retained UI element: geometry, visibility, opacity, children, and optional
// input handling. Rectangles and icons derive from it (see widgets.hpp).
class Node {
public:
    Node() = default;
    virtual ~Node();

    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    template <typename T, typename... Args>
    T* add(Args&&... args) {
        auto child = std::make_unique<T>(std::forward<Args>(args)...);
        T* raw = child.get();
        raw->parent_ = this;
        children_.push_back(std::move(child));
        return raw;
    }

    // ---- geometry (logical pixels, relative to the parent) ----
    float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
    Anchors anchors;
    bool visible = true;
    Animated<float> opacity{1.0f};
    bool clip = false;
    // Rounds the clip's corners.
    float clip_radius = 0.0f;

    // The size the node would like to be,
    // as opposed to the size it was given. A layout computes these from its
    // children, and a ScrollArea measures its content with them -- a column
    // anchored to fill its parent reports the parent's height, which says
    // nothing about how far its rows actually run.
    float implicit_width = 0.0f;
    float implicit_height = 0.0f;

    // Layout hints, read by LinearLayout. A negative preferred size means "use
    // whatever the node already has".
    struct LayoutHints {
        bool fill_width = false;
        bool fill_height = false;
        float preferred_width = -1.0f;
        float preferred_height = -1.0f;
    };
    LayoutHints layout_hints;

    // ---- input ----
    // A node only participates in hit testing if it has a handler. That keeps
    // decorative nodes out of the way without a separate "accepts input" flag.
    std::function<void()> on_click;
    std::function<void()> on_double_click;
    std::function<void(bool)> on_hover_changed;
    std::function<void(float, float)> on_press;    // absolute pointer position
    std::function<void()> on_release;
    // The right button, held rather than clicked.
    std::function<void(float, float)> on_right_press;
    std::function<void()> on_right_release;
    // The pointer moving over this node, pressed or not. A node that draws
    // many things inside itself -- a timeline ruler -- has to track its own
    // sub-hover, and needs the position to do it.
    std::function<void(float, float)> on_pointer_moved;
    // The middle button.
    std::function<void(float, float)> on_middle_press;
    std::function<void()> on_middle_release;
    // Fires while the pointer moves with the button held, whether or not it is
    // still inside the node -- a slider must keep tracking once grabbed, which
    // is why this is a drag rather than repeated hover.
    std::function<void(float, float)> on_drag;
    // External file drop (SDL_EVENT_DROP_FILE).
    std::function<void(const char*)> on_file_dropped;
    // Wheel and trackpad scroll. Deltas are in SDL's units; on macOS a trackpad
    // reports fractional precise deltas, a mouse reports whole notches.
    // Returning true consumes it, so a zooming canvas inside a scroll area does
    // not also scroll its parent.
    std::function<bool(float, float)> on_wheel;

    // ---- keyboard, routed to the focused node ----
    // Returning true consumes the key.
    std::function<bool(SDL_Keycode, SDL_Keymod)> on_key;
    std::function<void(const char*)> on_text_input;
    std::function<void(bool)> on_focus_changed;
    // Only a node that wants the keyboard takes focus, so clicking a plain
    // rectangle drops focus rather than stealing it.
    bool focusable = false;
    // Takes a drag away from whatever the press landed on, once the pointer
    // has travelled far enough to be a drag rather than a click. Without it a
    // list whose rows are clickable
    // cannot be scrolled by dragging at all: the press belongs to the row, and
    // the row has no idea it is inside something that scrolls.
    bool steals_drag = false;
    // Steals a drag that runs sideways instead, for something that scrolls
    // horizontally. Ignored unless steals_drag is set.
    bool steals_sideways = false;
    // Refuses to have its drag stolen. A scrollbar handle is a child of the
    // very thing that would steal from it, and a slider tracks a gesture that
    // has to stay its own however far it travels.
    bool keeps_drag = false;
    // A name for debugging only. Nothing in the program reads it.
    std::string debug_name;
    bool focused() const { return focused_; }

    bool hovered() const { return hovered_; }
    bool accepts_input() const {
        return on_click || on_double_click || on_hover_changed || on_press ||
               on_drag || on_file_dropped || on_wheel || on_right_press ||
               on_right_release || on_pointer_moved || on_middle_press;
    }

    // Absolute position, resolved during layout. Input handlers need it to map
    // a pointer position into node-local space without walking back up.
    float abs_x() const { return abs_x_; }
    float abs_y() const { return abs_y_; }

    // ---- tree ----
    Node* parent() const { return parent_; }
    const std::vector<std::unique_ptr<Node>>& children() const { return children_; }
    void clear_children() { children_.clear(); }
    // Moves this node to the end of its parent's children, so it paints last
    // and hit-tests first. A popup layer has to be on top of whatever was
    // added after it, and the alternative -- requiring every screen to build
    // its layer last -- is a rule that gets broken silently.
    void raise();

    // Resolves this node's geometry against `parent_w`/`parent_h`, then
    // recurses. Call on the root with the viewport size.
    void layout(float parent_w, float parent_h, float parent_abs_x = 0.0f,
                float parent_abs_y = 0.0f);

    // Advances every Animated in the subtree. Returns true if anything is
    // still in motion, which lets the caller decide whether another frame is
    // needed rather than redrawing unconditionally.
    bool tick(float dt);

    void paint(gpu::DrawList& draw, const PaintContext& ctx);

    // Front-to-back hit test. Returns the deepest input-accepting node
    // containing the point, or null. Coordinates are absolute logical pixels.
    Node* hit_test(float px, float py, float origin_x = 0.0f, float origin_y = 0.0f);

protected:
    // Subclasses draw themselves here; children are painted afterwards by the
    // base, so a parent never occludes its own children.
    virtual void paint_self(gpu::DrawList&, const PaintContext&) {}
    virtual bool tick_self(float) { return false; }

    // Called once this node's own geometry is resolved and before children
    // recurse. A component whose children are positioned by computation rather
    // than by anchors does that here, so the result is recomputed on every resize instead of
    // being frozen at construction.
    virtual void layout_self() {}

private:
    Node* parent_ = nullptr;
    std::vector<std::unique_ptr<Node>> children_;
    bool hovered_ = false;
    bool focused_ = false;
    float abs_x_ = 0.0f;
    float abs_y_ = 0.0f;

    friend class InputRouter;
};

// Turns pointer positions and clicks into hover/click callbacks on the tree.
//
// Hover is tracked as a single current target rather than a set: only the
// topmost node under the cursor is hovered, and routing it centrally is what
// makes enter/exit fire in the right order when the cursor crosses a boundary.
class InputRouter {
public:
    InputRouter();
    ~InputRouter();
    void set_pointer(float x, float y);
    void pointer_left();
    void press();
    void release();
    // The right button is routed to whatever is under the pointer and released
    // to the same node, so a hold that wanders off it still ends there.
    void press_right();
    void release_right();
    void press_middle();
    void release_middle();

    // Focus is owned here rather than by the tree so that exactly one node can
    // hold it, and so clicking anywhere can clear it.
    void set_focus(Node* node);
    // Called by ~Node so no router is left pointing at freed memory. The
    // router holds seven raw pointers into the tree -- hover, the press
    // targets, the last click, focus -- and a handler that deletes nodes,
    // which several do, would otherwise leave every one of them dangling.
    static void node_destroyed(Node* node);
    Node* focus() const { return focus_; }
    // A field that appears in response to a click wants the keyboard without
    // a second click on itself. It cannot call set_focus -- it has no way to
    // reach the router -- so it asks here and the next update obliges.
    static void request_focus(Node* node);
    bool handle_key(SDL_Keycode key, SDL_Keymod mods);
    void handle_text(const char* utf8);
    // Routed to whichever node is under the pointer at drop time.
    void file_dropped(const char* path, Node& root);
    // Delivered to the node under the pointer, then up the parent chain until
    // one consumes it.
    void wheel(float dx, float dy, Node& root);

    // Call once per frame, after layout.
    void update(Node& root);

    Node* hover_target() const { return hover_; }

private:
    float px_ = -1.0f, py_ = -1.0f;
    bool inside_ = false;
    bool pressed_ = false;
    bool press_pending_ = false;
    // Where the press landed, so the distance travelled can be measured, and
    // so the node that steals the drag can be told where the finger started
    // rather than where it had already got to.
    float press_x_ = 0.0f, press_y_ = 0.0f;
    bool drag_stolen_ = false;
    Node* hover_ = nullptr;
    Node* pressed_node_ = nullptr;
    Node* right_pressed_node_ = nullptr;
    Node* middle_pressed_node_ = nullptr;
    Uint64 last_click_ms_ = 0;
    Node* last_click_node_ = nullptr;
    Node* focus_ = nullptr;
};

}  // namespace ps::ui

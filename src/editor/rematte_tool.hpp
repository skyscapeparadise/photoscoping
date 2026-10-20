#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "editor/viewer.hpp"
#include "fixes/fixes.hpp"
#include "fixes/rematte.hpp"
#include "ui/node.hpp"

namespace ps::editor {

// Drawing and keyframing re-matte boxes, over the viewer.
//
//   drag outside a box   draws a new one, on this frame
//   drag inside a box    moves it, on this frame
//   drag a box's edge    resizes it from that side -- from a corner, both
//   K                    keys the selected box on this frame where it is: a
//                        box re-mattes from its first key to its last, so
//                        this is how one covers more of the clip
//   Backspace            the selected box's key on this frame, or the box if
//                        that was its only key
//   Shift-Backspace      the selected box, every key of it
//   Escape               lets go of the selection
//
// A box that is re-matted on this frame is drawn white; one still waiting
// for the run, lilac.
class RematteTool : public ui::Node {
public:
    void attach(Viewer* viewer, fixes::Fixes* fixes);
    void set_frame(std::int64_t frame) { frame_ = frame; }
    void set_active(bool active);
    bool active() const { return visible; }
    bool handle_key(SDL_Keycode key, SDL_Keymod mods);
    // For the card: the tool, and how much closer the selected box looks.
    std::string heading() const;

    std::function<void()> on_edit_begin;
    std::function<void()> on_live_change;
    std::function<void()> on_changed;
    // The selection, or the selected box's size, changed.
    std::function<void()> on_heading_changed;
    // Whether the patch for a box at a frame is made.
    std::function<bool(std::int64_t, const fixes::PixelRect&)> is_made;

protected:
    void paint_self(gpu::DrawList& draw, const ui::PaintContext& ctx) override;

private:
    enum class Drag { kNone, kPan, kDraw, kMove, kResize };
    // Which sides a press on a box took hold of.
    enum Side { kLeft = 1, kRight = 2, kTop = 4, kBottom = 8 };

    void press(float x, float y);
    void drag(float x, float y);
    void release();
    // The live box under (x, y), and the sides within reach; the selected
    // box first, then the newest.
    int hit_box(float x, float y, int* sides) const;
    void select(int id);
    void finish(bool edited);

    Viewer* viewer_ = nullptr;
    fixes::Fixes* fixes_ = nullptr;
    std::int64_t frame_ = 0;
    int selected_ = 0;

    Drag drag_ = Drag::kNone;
    int drag_id_ = 0;
    int sides_ = 0;
    float grab_x_ = 0.0f, grab_y_ = 0.0f;  // image pixels
    fixes::Point grabbed_[2];              // the box as it was: min, max
    bool edited_ = false;
};

}  // namespace ps::editor

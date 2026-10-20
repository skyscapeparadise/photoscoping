#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <functional>
#include <vector>

#include "editor/viewer.hpp"
#include "fixes/fixes.hpp"
#include "ui/node.hpp"

namespace ps::editor {

// Drawing and keyframing garbage mattes, over the viewer.
//
//   click             a point of a new shape, inside another or not; on the
//                     first point, or a double-click, or Return, closes it
//   drag a point      moves it, on this frame
//   drag an edge      moves the whole shape, on this frame. The edge rather
//                     than the inside: a garbage matte is drawn around the
//                     whole subject, so its inside is most of the frame, and
//                     a click there is more often the start of another shape
//                     than a wish to move this one
//   Backspace         drawing, the last point; otherwise the selected
//                     shape's key on this frame -- or the shape, if that
//                     was its only key
//   Shift-Backspace   the selected shape, every key of it
//   Escape            abandons the shape being drawn
//
// Every change is made on the current frame, which gets a key if it had
// none: the way a compositor's rotoscoping keys as you go. The fixes are the
// app's; this changes them and says so.
class GarbageTool : public ui::Node {
public:
    void attach(Viewer* viewer, fixes::Fixes* fixes);
    void set_frame(std::int64_t frame) { frame_ = frame; }
    // Whether the garbage tool is in hand. Out of hand this is not there at
    // all: it draws nothing and takes no input.
    void set_active(bool active);
    bool active() const { return visible; }
    // Return, Escape and Backspace. True if the key was this tool's.
    bool handle_key(SDL_Keycode key, SDL_Keymod mods);

    // Before any change, with the fixes as they were: for undo.
    std::function<void()> on_edit_begin;
    // During a drag, each time the shape moves: the mask follows live.
    std::function<void()> on_live_change;
    // When a change is complete: for saving.
    std::function<void()> on_changed;

protected:
    void paint_self(gpu::DrawList& draw, const ui::PaintContext& ctx) override;

private:
    enum class Drag { kNone, kPan, kVertex, kShape };

    void press(float x, float y);
    void drag(float x, float y);
    void release();
    void close_shape();
    // The point of a shape within reach of (x, y): shape id and index, or
    // false.
    bool hit_point(float x, float y, int* id, int* index) const;
    // The shape whose outline passes within reach of (x, y), or 0.
    int hit_edge(float x, float y) const;
    void begin_edit();

    Viewer* viewer_ = nullptr;
    fixes::Fixes* fixes_ = nullptr;
    std::int64_t frame_ = 0;

    std::vector<fixes::Point> drawing_;
    float pointer_x_ = 0.0f, pointer_y_ = 0.0f;
    int selected_ = 0;

    Drag drag_ = Drag::kNone;
    int drag_id_ = 0;
    int drag_index_ = 0;
    float grab_x_ = 0.0f, grab_y_ = 0.0f;  // image pixels
    std::vector<fixes::Point> grabbed_;
    bool edited_ = false;
};

}  // namespace ps::editor

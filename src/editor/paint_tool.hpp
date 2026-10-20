#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "editor/viewer.hpp"
#include "fixes/fixes.hpp"
#include "ui/node.hpp"

namespace ps::editor {

// Painting coverage out of the matte and back in, over the viewer.
//
//   drag              paints a stroke on this frame, in the current mode
//   Option-drag       paints the other way
//   X                 swaps the current mode
//   [ and ]           the brush smaller and larger
//   Command-drag      moves the stroke under the pointer, on this frame --
//                     which keys it there, so it tweens from its other keys
//   K                 keys the selected stroke on this frame where it is:
//                     a stroke is live from its first key to its last, so
//                     this is how one carries over more frames
//   Backspace         the selected stroke's key on this frame, or the
//                     stroke if that was its only key
//   Shift-Backspace   the selected stroke, every key of it
class PaintTool : public ui::Node {
public:
    void attach(Viewer* viewer, fixes::Fixes* fixes);
    void set_frame(std::int64_t frame) { frame_ = frame; }
    void set_active(bool active);
    bool active() const { return visible; }
    bool handle_key(SDL_Keycode key, SDL_Keymod mods);
    // For the card: the tool and its mode.
    std::string heading() const;

    std::function<void()> on_edit_begin;
    std::function<void()> on_live_change;
    std::function<void()> on_changed;
    // The mode or the brush changed, for whatever shows them.
    std::function<void()> on_brush_changed;

protected:
    void paint_self(gpu::DrawList& draw, const ui::PaintContext& ctx) override;

private:
    enum class Drag { kNone, kPan, kPaint, kMove };

    void press(float x, float y);
    void drag(float x, float y);
    void release();
    int hit_stroke(float x, float y) const;
    void finish(bool edited);

    Viewer* viewer_ = nullptr;
    fixes::Fixes* fixes_ = nullptr;
    std::int64_t frame_ = 0;
    fixes::PaintMode mode_ = fixes::PaintMode::kOut;
    float radius_ = 24.0f;  // image pixels
    int selected_ = 0;

    Drag drag_ = Drag::kNone;
    int drag_id_ = 0;
    std::vector<fixes::Point> stroke_;
    float grab_x_ = 0.0f, grab_y_ = 0.0f;
    std::vector<fixes::Point> grabbed_;
    bool edited_ = false;
    float pointer_x_ = -1.0f, pointer_y_ = -1.0f;
};

}  // namespace ps::editor

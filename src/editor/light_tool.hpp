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

// Drawing and keyframing light-map crops (light/light_map.hpp), over the
// viewer: four corners on the backdrop each, straightened on export.
//
//   drag on the backdrop   draws a new crop, square to the frame
//   drag a corner          moves it, on this frame: pull the crop into the
//                          shape of the board it is on
//   drag inside a crop     moves it, on this frame
//   K                      keys the selected crop on this frame
//   Backspace              the selected crop's key on this frame, or the crop
//                          if that was its only key
//   Shift-Backspace        the selected crop, every key of it
//   Escape                 lets go of the selection
//
// A crop holds before its first key and after its last, like a garbage
// matte: on a locked-off shot it is drawn once and stays.
class LightTool : public ui::Node {
public:
    void attach(Viewer* viewer, fixes::Fixes* fixes);
    void set_frame(std::int64_t frame) { frame_ = frame; }
    void set_active(bool active);
    bool active() const { return visible; }
    bool handle_key(SDL_Keycode key, SDL_Keymod mods);
    // For the card: the selected crop's name.
    std::string heading() const;
    int selected() const { return selected_; }
    // After an undo, a selection that no longer exists is let go.
    void forget_missing();

    std::function<void()> on_edit_begin;
    std::function<void()> on_live_change;
    std::function<void()> on_changed;
    // The selection changed.
    std::function<void()> on_selection_changed;

protected:
    void paint_self(gpu::DrawList& draw, const ui::PaintContext& ctx) override;

private:
    enum class Drag { kNone, kPan, kDraw, kCorner, kMove };

    void press(float x, float y);
    void drag(float x, float y);
    void release();
    void select(int id);
    // The crop and corner under (x, y): a corner if one is within reach,
    // else -1 and a crop if the point is inside one; the selected first.
    int hit(float x, float y, int* corner) const;

    Viewer* viewer_ = nullptr;
    fixes::Fixes* fixes_ = nullptr;
    std::int64_t frame_ = 0;
    int selected_ = 0;

    Drag drag_ = Drag::kNone;
    int drag_id_ = 0;
    int corner_ = -1;
    float grab_x_ = 0.0f, grab_y_ = 0.0f;  // image pixels
    std::vector<fixes::Point> grabbed_;
    bool edited_ = false;
};

}  // namespace ps::editor

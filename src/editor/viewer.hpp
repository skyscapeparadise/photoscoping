#pragma once

#include <functional>

#include "gpu/draw_list.hpp"
#include "ui/font.hpp"
#include "ui/node.hpp"
#include "ui/text.hpp"

namespace ps::editor {

// The viewer's area: where the footage is drawn, and how it is zoomed and
// panned. The footage itself is a GPU pass of its own (gpu::DisplayPass);
// this node holds the view and takes the gestures, and the app asks it where
// the image goes.
//
// The view is "fit" -- the whole frame, as large as the area allows, kept so
// as the window resizes -- until it is zoomed or panned, after which it holds
// its own scale and centre. Scale is logical pixels per image pixel.
class Viewer : public ui::Node {
public:
    void build(ui::FontCache& fonts);

    // The footage's size in pixels; zero for none.
    void set_image_size(int width, int height);
    int image_width() const { return image_w_; }
    int image_height() const { return image_h_; }
    // Whether drags pan: the navigate tool is in hand.
    void set_navigating(bool navigating);
    // Space is held: drags pan whatever the tool.
    void set_space_held(bool held);
    // Whether a drag panned while space was held. Space is also play and
    // pause, so it acts on release, and only if it was not used to pan.
    bool space_panned() const { return space_panned_; }
    // Device pixels per logical pixel, from the app each frame.
    void set_device_scale(float scale) { device_scale_ = scale > 0.0f ? scale : 1.0f; }

    void fit();
    // One image pixel to one device pixel.
    void actual_pixels();
    // Zooms by `factor` about a point in absolute logical coordinates.
    void zoom_at(float factor, float x, float y);
    // Zooms by `factor` about the area's centre.
    void zoom_by(float factor);

    // Where the whole image lands, in absolute logical pixels. Runs past the
    // area when zoomed in; the display pass clips to the area.
    gpu::Rect image_rect() const;
    // Image pixels per device pixel above which the footage is sampled
    // nearest-neighbour, so single pixels read as squares when inspected.
    bool magnified() const;
    // Absolute logical coordinates to image pixels, and back.
    void to_image(float x, float y, float* ix, float* iy) const;
    void from_image(float ix, float iy, float* x, float* y) const;
    // Logical pixels per image pixel.
    float scale() const { return scale_; }
    bool space_held() const { return space_held_; }

    // A pan driven from outside: a tool over the viewer that has the pointer
    // hands its space- and middle-drags here.
    void begin_pan(float x, float y);
    void pan_to(float x, float y);
    void end_pan();

protected:
    void layout_self() override;

private:
    float fit_scale() const;
    void clamp_centre();
    void refresh_readout();

    int image_w_ = 0, image_h_ = 0;
    bool fitted_ = true;
    float scale_ = 1.0f;                     // logical px per image px
    float centre_x_ = 0.0f, centre_y_ = 0.0f;  // image px at the area's centre
    bool navigating_ = false;
    bool space_held_ = false;
    bool space_panned_ = false;
    float device_scale_ = 1.0f;
    bool panning_ = false;
    float last_x_ = 0.0f, last_y_ = 0.0f;
    ui::TextNode* readout_ = nullptr;
};

}  // namespace ps::editor

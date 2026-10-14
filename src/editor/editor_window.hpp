#pragma once

#include "editor/inspector.hpp"
#include "editor/timeline.hpp"
#include "gpu/icon_atlas.hpp"
#include "ui/font.hpp"
#include "ui/node.hpp"
#include "ui/widgets.hpp"

namespace ps::editor {

// The whole window: the viewer, the inspector to its right, and the timeline
// along the bottom.
//
// Understory lays its editor out once, at a fixed size, and scales the result
// to the window. This one is laid out against the window as it actually is.
// The viewer takes whatever the others leave; the inspector keeps a width the
// user can drag between a floor and a ceiling; and when the window is too
// narrow for both, the inspector slides away and gives the footage everything.
class EditorWindow : public ui::Node {
public:
    // The hands come from `large_icons`, the transport's glyphs from
    // `small_icons`: the same art, rasterized for each size.
    void build(ui::FontCache& fonts, const gpu::IconAtlas& large_icons,
               const gpu::IconAtlas& small_icons);

    Inspector* inspector() const { return inspector_; }
    Timeline* timeline() const { return timeline_; }

    // In logical pixels, as of the last layout. The footage and the nebula are
    // GPU passes of their own, drawn into these before the interface.
    gpu::Rect viewer_rect() const;
    // The part of the inspector on screen. While it slides, that is less than
    // its full width, which is panel_width().
    gpu::Rect panel_rect() const;
    float panel_width() const;

    // Shows or hides the inspector. Hidden by the user it stays hidden however
    // wide the window gets; hidden for want of room it comes back on its own.
    void toggle_panel();

protected:
    void layout_self() override;
    bool tick_self(float dt) override;

private:
    void refresh_splitter_look();

    Inspector* inspector_ = nullptr;
    Timeline* timeline_ = nullptr;
    ui::Node* splitter_ = nullptr;
    ui::RectNode* splitter_line_ = nullptr;

    // What the user asked for by dragging, clamped at layout. Until they do,
    // negative: the panel keeps a share of the window instead, so it grows
    // and shrinks with it.
    float panel_width_ = -1.0f;
    float laid_panel_width_ = 0.0f;
    bool panel_open_ = true;
    bool forced_ = false;
    bool dragging_splitter_ = false;
    // How much of the inspector is on screen, 0..1. Eased, so it slides.
    ui::Animated<float> reveal_{1.0f};
};

}  // namespace ps::editor

#include "editor/editor_window.hpp"

#include <SDL3/SDL.h>

#include <algorithm>

#include "editor/theme.hpp"

namespace ps::editor {

namespace {

constexpr float kMinPanel = 300.0f;
constexpr float kMaxPanel = 600.0f;
// The inspector never takes more than this share of the window, so the
// footage is always the larger part of it.
constexpr float kMaxPanelShare = 0.45f;
// Below this the footage would be squeezed under 500px beside a minimal
// panel, and the panel goes.
constexpr float kCollapseBelow = kMinPanel + 500.0f;
// The panel's share of the window until the user sets a width of their own.
constexpr float kDefaultPanelShare = 0.27f;
// The grab zone either side of the viewer's edge. Wider than anything drawn,
// because a two pixel target is a frustrating one.
constexpr float kSplitterGrab = 5.0f;

SDL_Cursor* resize_cursor() {
    static SDL_Cursor* cursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_EW_RESIZE);
    return cursor;
}

}  // namespace

void EditorWindow::build(ui::FontCache& fonts, const gpu::IconAtlas& large_icons,
                         const gpu::IconAtlas& small_icons) {
    anchors = ui::Anchors::Fill();
    reveal_.set_behavior(0.24f, ui::Easing::kInOutCubic);

    // First, so the panel and the timeline are over it where they meet.
    viewer_ = add<Viewer>();
    viewer_->build(fonts);

    inspector_ = add<Inspector>();
    inspector_->build(fonts, large_icons);
    inspector_->on_tool_changed = [this](Tool tool) {
        viewer_->set_navigating(tool == Tool::kNavigate);
    };
    inspector_->tools()->on_preview = [this] { toggle_panel(); };

    timeline_ = add<Timeline>();
    timeline_->build(fonts, small_icons);

    // Last, so it is on top of both panels it straddles.
    splitter_ = add<ui::Node>();
    splitter_->keeps_drag = true;
    splitter_line_ = splitter_->add<ui::RectNode>();
    splitter_line_->fill.set_behavior(0.12f, ui::Easing::kInOutQuad);
    splitter_line_->fill.reset(theme::kClear);
    splitter_->on_hover_changed = [this](bool inside) {
        if (!dragging_splitter_) SDL_SetCursor(inside ? resize_cursor() : SDL_GetDefaultCursor());
        refresh_splitter_look();
    };
    splitter_->on_press = [this](float, float) {
        dragging_splitter_ = true;
        refresh_splitter_look();
    };
    splitter_->on_drag = [this](float px, float) {
        if (dragging_splitter_) panel_width_ = width - px;
    };
    splitter_->on_release = [this] {
        dragging_splitter_ = false;
        // Clamped at layout every frame; keeping the clamped value means a
        // drag past the limit does not have to be undone before it responds.
        panel_width_ = laid_panel_width_;
        if (!splitter_->hovered()) SDL_SetCursor(SDL_GetDefaultCursor());
        refresh_splitter_look();
    };
}

void EditorWindow::toggle_panel() {
    // Asked for in a window too narrow to have it on its own, it comes anyway
    // and the footage gives up the room, until the window is resized.
    panel_open_ = reveal_.target() < 0.5f;
    forced_ = panel_open_;
    reveal_ = panel_open_ ? 1.0f : 0.0f;
}

void EditorWindow::refresh_splitter_look() {
    const bool lit = dragging_splitter_ || splitter_->hovered();
    splitter_line_->fill = lit ? theme::kHover : theme::kClear;
}

bool EditorWindow::tick_self(float dt) { return reveal_.tick(dt); }

void EditorWindow::layout_self() {
    // A window that shrinks below the breakpoint hides the panel; growing
    // past it again brings the panel back, unless the user put it away.
    const bool room = width >= kCollapseBelow;
    if (room) forced_ = false;
    const float wanted = (panel_open_ && (room || forced_)) ? 1.0f : 0.0f;
    if (wanted != reveal_.target()) reveal_ = wanted;

    const float content_h = std::max(0.0f, height - Timeline::kHeight);
    const float ceiling = std::max(kMinPanel, std::min(kMaxPanel, width * kMaxPanelShare));
    const float asked = panel_width_ >= 0.0f ? panel_width_ : width * kDefaultPanelShare;
    laid_panel_width_ = std::clamp(asked, kMinPanel, ceiling);
    const float shown = laid_panel_width_ * reveal_.value();

    inspector_->x = width - shown;
    inspector_->y = 0.0f;
    inspector_->width = laid_panel_width_;
    inspector_->height = content_h;
    inspector_->visible = shown > 0.5f;

    splitter_->x = width - shown - kSplitterGrab;
    splitter_->y = 0.0f;
    splitter_->width = kSplitterGrab * 2.0f;
    splitter_->height = content_h;
    splitter_->visible = inspector_->visible && reveal_.value() >= 1.0f;
    splitter_line_->x = kSplitterGrab - 1.0f;
    splitter_line_->width = 2.0f;
    splitter_line_->height = content_h;

    const gpu::Rect view = viewer_rect();
    viewer_->x = view.x;
    viewer_->y = view.y;
    viewer_->width = view.w;
    viewer_->height = view.h;

    timeline_->x = 0.0f;
    timeline_->y = content_h;
    timeline_->width = width;
    timeline_->height = Timeline::kHeight;
}

gpu::Rect EditorWindow::viewer_rect() const {
    const float content_h = std::max(0.0f, height - Timeline::kHeight);
    return gpu::Rect{0.0f, 0.0f, std::max(0.0f, width - laid_panel_width_ * reveal_.value()),
                     content_h};
}

gpu::Rect EditorWindow::panel_rect() const {
    if (!inspector_->visible) return gpu::Rect{};
    return gpu::Rect{inspector_->x, inspector_->y, laid_panel_width_ * reveal_.value(),
                     inspector_->height};
}

float EditorWindow::panel_width() const { return laid_panel_width_; }

}  // namespace ps::editor

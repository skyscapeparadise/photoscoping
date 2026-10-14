#include "editor/tool_strip.hpp"

#include <algorithm>

#include "editor/theme.hpp"

namespace ps::editor {

namespace {

constexpr int kButtons = 5;
constexpr float kMinButton = 44.0f;
constexpr float kMaxButton = 84.0f;
constexpr float kMinGap = 8.0f;
// Understory's proportions: a 56px hand in a 72px button with 12px corners.
constexpr float kIconRatio = 56.0f / 72.0f;
constexpr float kRadiusRatio = 12.0f / 72.0f;
constexpr float kBorder = 2.0f;

float button_size(float width) {
    return std::clamp((width - kMinGap * (kButtons - 1)) / kButtons, kMinButton, kMaxButton);
}

}  // namespace

const char* tool_label(Tool tool) {
    switch (tool) {
        case Tool::kSelect: return "select";
        case Tool::kNavigate: return "navigate";
        case Tool::kRegion: return "region";
        case Tool::kErase: return "erase";
    }
    return "select";
}

float ToolStrip::height_for(float width) { return button_size(width); }

void ToolStrip::build(const gpu::IconAtlas& atlas) {
    // The hands are understory's own art, borrowed where the gesture means the
    // same thing here: pointing to select, a pinch to move around the frame, a
    // finger dragging out a box for a region, a crumpling hand to erase, and
    // the landscape for preview.
    const struct { const char* icon; Tool tool; bool preview; } kEntries[kButtons] = {
        {"select", Tool::kSelect, false},
        {"pinch", Tool::kNavigate, false},
        {"newarea", Tool::kRegion, false},
        {"destroy", Tool::kErase, false},
        {"preview", Tool::kSelect, true},
    };

    for (const auto& entry : kEntries) {
        Button button;
        button.tool = entry.tool;
        button.is_preview = entry.preview;

        button.root = add<ui::Node>();
        button.frame = button.root->add<ui::RectNode>();
        button.frame->anchors = ui::Anchors::Fill();
        button.frame->border_width = kBorder;
        button.frame->border_color.set_behavior(0.08f, ui::Easing::kInOutQuad);
        button.frame->border_color.reset(theme::kWhite);
        button.frame->fill.set_behavior(0.12f, ui::Easing::kInOutQuad);
        button.frame->fill.reset(theme::kClear);

        button.icon = button.root->add<ui::IconNode>();
        button.icon->atlas = &atlas;
        button.icon->icon_name = entry.icon;
        button.icon->fill_mode = ui::IconNode::FillMode::kFit;
        button.icon->colorize = true;
        button.icon->tint.set_behavior(0.12f, ui::Easing::kInOutQuad);
        button.icon->tint.reset(theme::kWhite);

        buttons_.push_back(button);
    }

    // Wired after the vector is filled, so the captured index stays valid.
    for (std::size_t i = 0; i < buttons_.size(); ++i) {
        buttons_[i].root->on_hover_changed = [this, i](bool inside) {
            buttons_[i].frame->border_color = inside ? theme::kHover : theme::kWhite;
        };
        buttons_[i].root->on_click = [this, i] {
            const Button& hit = buttons_[i];
            if (hit.is_preview) {
                if (on_preview) on_preview();
                return;
            }
            set_tool(hit.tool);
        };
    }
    refresh_states();
}

void ToolStrip::layout_self() {
    const float size = button_size(width);
    // Once the buttons stop growing the gaps take what is left, so the row
    // still runs edge to edge under the card.
    const float gap = kButtons > 1 ? std::max(kMinGap, (width - size * kButtons) / (kButtons - 1))
                                   : 0.0f;
    const float icon = size * kIconRatio;
    for (std::size_t i = 0; i < buttons_.size(); ++i) {
        Button& b = buttons_[i];
        b.root->x = static_cast<float>(i) * (size + gap);
        b.root->y = 0.0f;
        b.root->width = size;
        b.root->height = size;
        b.frame->radius = size * kRadiusRatio;
        b.icon->x = (size - icon) * 0.5f;
        b.icon->y = (size - icon) * 0.5f;
        b.icon->width = icon;
        b.icon->height = icon;
    }
}

void ToolStrip::set_tool(Tool tool) {
    if (tool_ == tool) return;
    tool_ = tool;
    refresh_states();
    if (on_tool_changed) on_tool_changed(tool_);
}

void ToolStrip::refresh_states() {
    for (Button& button : buttons_) {
        const bool active = !button.is_preview && button.tool == tool_;
        button.frame->fill = active ? theme::kWhite : theme::kClear;
        button.icon->tint = active ? theme::kActiveIcon : theme::kWhite;
    }
}

}  // namespace ps::editor

#include "editor/inspector.hpp"

#include <algorithm>

#include "editor/theme.hpp"

namespace ps::editor {

namespace {

constexpr float kMinMargin = 16.0f;
constexpr float kMaxMargin = 28.0f;
constexpr float kCardRadius = 24.0f;
constexpr float kCardBorder = 2.0f;
constexpr float kRowHeight = 30.0f;

}  // namespace

void Inspector::build(ui::FontCache& fonts, const gpu::IconAtlas& atlas) {
    fonts_ = &fonts;

    tools_ = add<ToolStrip>();
    tools_->build(atlas, fonts);
    tools_->on_tool_changed = [this](Tool tool) {
        heading_->set_text(tool_label(tool));
        if (on_tool_changed) on_tool_changed(tool);
    };

    card_ = add<ui::RectNode>();
    card_->border_width = kCardBorder;
    card_->border_color = theme::kWhite;
    card_->fill = theme::kClear;
    card_->radius = kCardRadius;
    card_->clip = true;
    card_->clip_radius = kCardRadius;

    // The same three tiers as understory's card -- the tool in bold, the
    // subject large beneath it, a hairline, then what the subject is.
    heading_ = card_->add<ui::TextNode>();
    heading_->set_font_cache(&fonts);
    heading_->set_text(tool_label(tools_->tool()));
    heading_->bold = true;
    heading_->color = theme::kWhite;
    heading_->elide = ui::Elide::kRight;

    title_ = card_->add<ui::TextNode>();
    title_->set_font_cache(&fonts);
    title_->bold = true;
    title_->color = theme::kWhite;
    title_->elide = ui::Elide::kRight;
    title_->v_align = ui::VAlign::kBottom;

    rule_ = card_->add<ui::RectNode>();
    rule_->height = 1.5f;
    rule_->fill = theme::kWhite;

    caption_ = card_->add<ui::TextNode>();
    caption_->set_font_cache(&fonts);
    caption_->set_text("clip");
    caption_->pixel_size = 17.0f;
    caption_->color = theme::kWhite;

    rows_ = card_->add<ui::Node>();

    views_ = add<ViewSwitch>();
    views_->build(fonts);

    action_ = add<ui::RectNode>();
    action_->height = ViewSwitch::kPillHeight;
    action_->radius = ViewSwitch::kPillHeight * 0.5f;
    action_->border_width = 2.0f;
    action_->border_color.set_behavior(0.08f, ui::Easing::kInOutQuad);
    action_label_ = action_->add<ui::TextNode>();
    action_label_->set_font_cache(&fonts);
    action_label_->pixel_size = 18.0f;
    action_label_->anchors = ui::Anchors::Fill();
    action_label_->h_align = ui::HAlign::kCenter;
    action_label_->v_align = ui::VAlign::kCenter;
    action_->on_hover_changed = [this](bool) { refresh_action(); };
    action_->on_click = [this] {
        if (action_enabled_ && on_action) on_action();
    };
    set_action("matte clip", false);

    set_clip(ClipInfo{"no clip", {}});
}

void Inspector::set_clip(const ClipInfo& info) {
    info_ = info;
    title_->set_text(info_.name);
    rebuild_rows();
}

void Inspector::set_row(const std::string& label, const std::string& value) {
    for (std::size_t i = 0; i < info_.rows.size(); ++i) {
        if (info_.rows[i].first != label) continue;
        info_.rows[i].second = value;
        if (i < row_nodes_.size()) row_nodes_[i].value->set_text(value);
        return;
    }
}

void Inspector::set_action(const std::string& label, bool enabled) {
    action_label_->set_text(label);
    action_enabled_ = enabled;
    refresh_action();
}

void Inspector::refresh_action() {
    const gpu::Color c = !action_enabled_     ? theme::kDormant
                         : action_->hovered() ? theme::kHover
                                              : theme::kWhite;
    action_->border_color = c;
    action_label_->color = action_enabled_ ? theme::kWhite : theme::kDormant;
}

void Inspector::rebuild_rows() {
    rows_->clear_children();
    row_nodes_.clear();
    for (const auto& [label_text, value_text] : info_.rows) {
        Row row;
        row.label = rows_->add<ui::TextNode>();
        row.label->set_font_cache(fonts_);
        row.label->set_text(label_text);
        row.label->pixel_size = 15.0f;
        row.label->color = gpu::Color::hex(0xFFFFFF, 0.6f);
        row.label->v_align = ui::VAlign::kCenter;
        row.value = rows_->add<ui::TextNode>();
        row.value->set_font_cache(fonts_);
        row.value->set_text(value_text);
        row.value->pixel_size = 15.0f;
        row.value->color = theme::kWhite;
        row.value->h_align = ui::HAlign::kRight;
        row.value->v_align = ui::VAlign::kCenter;
        row.value->elide = ui::Elide::kMiddle;
        row_nodes_.push_back(row);
    }
}

void Inspector::layout_self() {
    // Margins breathe with the panel, so a narrow one is not mostly gutter.
    const float margin = std::clamp(width * 0.055f, kMinMargin, kMaxMargin);
    const float inner = std::max(0.0f, width - margin * 2.0f);

    tools_->x = margin;
    tools_->y = margin;
    tools_->width = inner;
    tools_->height = ToolStrip::height_for(inner);

    card_->x = margin;
    card_->y = tools_->y + tools_->height + margin;
    card_->width = inner;
    views_->x = margin;
    views_->width = inner;
    views_->height = ViewSwitch::kHeight;
    views_->y = std::max(card_->y, height - margin - ViewSwitch::kHeight);
    action_->x = margin;
    action_->width = inner;
    action_->y = std::max(card_->y, views_->y - ViewSwitch::kGap - ViewSwitch::kPillHeight);
    card_->height = std::max(0.0f, action_->y - margin - card_->y);

    // Inside the card. Type scales with the card's width -- the clip's name
    // is the one thing here meant to be read from across the room.
    const float pad = std::clamp(inner * 0.07f, 18.0f, 30.0f);
    const float text_w = std::max(0.0f, inner - pad * 2.0f);
    const float heading_size = std::clamp(inner * 0.075f, 22.0f, 30.0f);
    const float title_size = std::clamp(inner * 0.13f, 28.0f, 56.0f);

    heading_->pixel_size = heading_size;
    heading_->x = pad;
    heading_->y = pad;
    heading_->width = text_w;
    heading_->height = heading_size * 1.4f;

    title_->pixel_size = title_size;
    title_->x = pad;
    title_->y = heading_->y + heading_->height + pad * 0.6f;
    title_->width = text_w;
    title_->height = title_size * 1.35f;

    rule_->x = pad;
    rule_->y = title_->y + title_->height + 6.0f;
    rule_->width = text_w;

    caption_->x = pad;
    caption_->y = rule_->y + 10.0f;
    caption_->width = text_w;
    caption_->height = 24.0f;

    rows_->x = pad;
    rows_->y = caption_->y + caption_->height + pad * 0.8f;
    rows_->width = text_w;
    rows_->height = std::max(0.0f, card_->height - rows_->y - pad * 0.5f);

    // A label column wide enough for the longest label in use, measured
    // roughly rather than shaped: these are short lowercase words.
    std::size_t longest = 0;
    for (const auto& row : info_.rows) longest = std::max(longest, row.first.size());
    const float label_w = std::min(text_w * 0.45f, 9.0f * static_cast<float>(longest) + 12.0f);
    for (std::size_t i = 0; i < row_nodes_.size(); ++i) {
        const float y = static_cast<float>(i) * kRowHeight;
        const bool fits = y + kRowHeight <= rows_->height;
        Row& row = row_nodes_[i];
        row.label->visible = fits;
        row.value->visible = fits;
        row.label->x = 0.0f;
        row.label->y = y;
        row.label->width = label_w;
        row.label->height = kRowHeight;
        row.value->x = label_w;
        row.value->y = y;
        row.value->width = std::max(0.0f, text_w - label_w);
        row.value->height = kRowHeight;
    }
}

}  // namespace ps::editor

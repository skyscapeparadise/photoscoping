#include "editor/light_controls.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "editor/theme.hpp"

namespace ps::editor {

namespace {

constexpr float kRow = 34.0f;
constexpr float kLabelW = 86.0f;
constexpr float kReadoutW = 42.0f;
constexpr float kKnob = 12.0f;

}  // namespace

void Slider::build(ui::FontCache& fonts, const char* label) {
    label_ = add<ui::TextNode>();
    label_->set_font_cache(&fonts);
    label_->set_text(label);
    label_->pixel_size = 15.0f;
    label_->color = gpu::Color::hex(0xFFFFFF, 0.6f);
    label_->v_align = ui::VAlign::kCenter;
    readout_ = add<ui::TextNode>();
    readout_->set_font_cache(&fonts);
    readout_->pixel_size = 15.0f;
    readout_->color = theme::kWhite;
    readout_->h_align = ui::HAlign::kRight;
    readout_->v_align = ui::VAlign::kCenter;
    track_ = add<ui::RectNode>();
    track_->fill = gpu::Color::hex(0xFFFFFF, 0.25f);
    track_->radius = 1.0f;
    fill_ = add<ui::RectNode>();
    fill_->fill = theme::kHover;
    fill_->radius = 1.0f;
    knob_ = add<ui::RectNode>();
    knob_->fill = theme::kWhite;
    knob_->radius = kKnob * 0.5f;

    keeps_drag = true;
    on_press = [this](float x, float) {
        dragging_ = true;
        if (on_begin) on_begin();
        set_from(x, false);
    };
    on_drag = [this](float x, float) {
        if (dragging_) set_from(x, false);
    };
    on_release = [this] {
        if (!dragging_) return;
        dragging_ = false;
        if (on_change) on_change(value_, true);
    };
    on_double_click = [this] {
        if (on_begin) on_begin();
        set_value(default_);
        if (on_change) on_change(value_, true);
    };
    set_value(0.0f);
}

void Slider::set_value(float v) {
    value_ = std::clamp(v, lo_, hi_);
    char text[16];
    std::snprintf(text, sizeof text, "%+.0f", value_);
    // Ranges that start at zero read without a sign.
    readout_->set_text(lo_ >= 0.0f ? text + (text[0] == '+' ? 1 : 0) : text);
    layout_self();
}

void Slider::set_from(float x, bool done) {
    const float x0 = abs_x() + kLabelW, w = std::max(1.0f, width - kLabelW - kReadoutW - 10.0f);
    const float t = std::clamp((x - x0) / w, 0.0f, 1.0f);
    set_value(std::round(lo_ + t * (hi_ - lo_)));
    if (on_change) on_change(value_, done);
}

void Slider::layout_self() {
    if (!label_) return;
    label_->x = 0.0f;
    label_->y = 0.0f;
    label_->width = kLabelW;
    label_->height = height;
    readout_->x = width - kReadoutW;
    readout_->y = 0.0f;
    readout_->width = kReadoutW;
    readout_->height = height;
    const float tx = kLabelW, tw = std::max(1.0f, width - kLabelW - kReadoutW - 10.0f);
    track_->x = tx;
    track_->y = height * 0.5f - 1.0f;
    track_->width = tw;
    track_->height = 2.0f;
    // Filled from the default -- zero, mostly -- to the value.
    const float t = (value_ - lo_) / std::max(1e-6f, hi_ - lo_);
    const float t0 = (default_ - lo_) / std::max(1e-6f, hi_ - lo_);
    fill_->x = tx + tw * std::min(t, t0);
    fill_->y = track_->y;
    fill_->width = tw * std::fabs(t - t0);
    fill_->height = 2.0f;
    knob_->x = tx + tw * t - kKnob * 0.5f;
    knob_->y = height * 0.5f - kKnob * 0.5f;
    knob_->width = kKnob;
    knob_->height = kKnob;
}

void LightControls::build(ui::FontCache& fonts) {
    hue_ = add<Slider>();
    hue_->build(fonts, "hue");
    saturation_ = add<Slider>();
    saturation_->build(fonts, "saturation");
    lightness_ = add<Slider>();
    lightness_->build(fonts, "lightness");
    for (Slider* s : {hue_, saturation_, lightness_}) {
        s->on_begin = [this] {
            if (bound_ && on_edit_begin) on_edit_begin();
        };
        s->on_change = [this](float, bool done) { apply(done); };
    }

    colorize_ = add<ui::RectNode>();
    colorize_->radius = 13.0f;
    colorize_->border_width = 2.0f;
    colorize_->border_color = theme::kWhite;
    colorize_label_ = colorize_->add<ui::TextNode>();
    colorize_label_->set_font_cache(&fonts);
    colorize_label_->set_text("colorize");
    colorize_label_->pixel_size = 15.0f;
    colorize_label_->anchors = ui::Anchors::Fill();
    colorize_label_->h_align = ui::HAlign::kCenter;
    colorize_label_->v_align = ui::VAlign::kCenter;
    colorize_->on_click = [this] {
        if (!bound_) return;
        if (on_edit_begin) on_edit_begin();
        crop_.colorize = !crop_.colorize;
        // Photoshop's starting point for each: colorized, a hue of the
        // wheel and a quarter saturation; back, no change at all.
        crop_.hue = crop_.colorize ? 30.0f : 0.0f;
        crop_.saturation = crop_.colorize ? 25.0f : 0.0f;
        crop_.lightness = 0.0f;
        bind(&crop_, size_->visible ? size_text_cache_ : std::string());
        apply(true);
    };

    size_ = add<ui::TextNode>();
    size_->set_font_cache(&fonts);
    size_->pixel_size = 15.0f;
    size_->color = gpu::Color::hex(0xFFFFFF, 0.6f);
    size_->v_align = ui::VAlign::kCenter;

    hint_ = add<ui::TextNode>();
    hint_->set_font_cache(&fonts);
    hint_->pixel_size = 15.0f;
    hint_->color = gpu::Color::hex(0xFFFFFF, 0.6f);
    hint_->set_text("drag on the backdrop to add a light");
    hint_->v_align = ui::VAlign::kCenter;

    bind(nullptr, {});
}

void LightControls::bind(const fixes::LightCrop* crop, const std::string& size_text) {
    bound_ = crop != nullptr;
    if (crop && crop != &crop_) crop_ = *crop;
    size_text_cache_ = size_text;
    const bool c = bound_ && crop_.colorize;
    hue_->set_range(c ? 0.0f : -180.0f, c ? 360.0f : 180.0f, c ? 30.0f : 0.0f);
    saturation_->set_range(c ? 0.0f : -100.0f, 100.0f, c ? 25.0f : 0.0f);
    lightness_->set_range(-100.0f, 100.0f, 0.0f);
    hue_->set_value(bound_ ? crop_.hue : 0.0f);
    saturation_->set_value(bound_ ? crop_.saturation : 0.0f);
    lightness_->set_value(bound_ ? crop_.lightness : 0.0f);
    for (Slider* s : {hue_, saturation_, lightness_}) s->visible = bound_;
    colorize_->visible = bound_;
    size_->visible = bound_;
    size_->set_text(size_text);
    hint_->visible = !bound_;
    refresh_toggle();
    layout_self();
}

void LightControls::apply(bool done) {
    if (!bound_) return;
    crop_.hue = hue_->value();
    crop_.saturation = saturation_->value();
    crop_.lightness = lightness_->value();
    if (on_adjust) on_adjust(crop_, done);
}

void LightControls::refresh_toggle() {
    const bool on = bound_ && crop_.colorize;
    colorize_->fill = on ? theme::kWhite : theme::kClear;
    colorize_label_->color = on ? theme::kActiveIcon : theme::kWhite;
}

void LightControls::layout_self() {
    if (!hue_) return;
    float y = 0.0f;
    for (Slider* s : {hue_, saturation_, lightness_}) {
        s->x = 0.0f;
        s->y = y;
        s->width = width;
        s->height = kRow;
        y += kRow;
    }
    colorize_->x = 0.0f;
    colorize_->y = y + 8.0f;
    colorize_->width = std::min(width, 120.0f);
    colorize_->height = 26.0f;
    size_->x = 0.0f;
    size_->y = y + 44.0f;
    size_->width = width;
    size_->height = 24.0f;
    hint_->x = 0.0f;
    hint_->y = 0.0f;
    hint_->width = width;
    hint_->height = kRow;
}

}  // namespace ps::editor

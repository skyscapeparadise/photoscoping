#include "editor/view_switch.hpp"

#include <algorithm>

#include "editor/theme.hpp"

namespace ps::editor {

const char* view_label(View view) {
    switch (view) {
        case View::kFootage: return "footage";
        case View::kMatte: return "matte";
        case View::kSubject: return "subject";
        case View::kOverlay: return "overlay";
    }
    return "footage";
}

void ViewSwitch::build(ui::FontCache& fonts) {
    for (View view : {View::kFootage, View::kMatte, View::kSubject, View::kOverlay}) {
        Pill pill;
        pill.view = view;
        pill.frame = add<ui::RectNode>();
        pill.frame->radius = kPillHeight * 0.5f;
        pill.frame->border_width = 2.0f;
        pill.frame->border_color.set_behavior(0.08f, ui::Easing::kInOutQuad);
        pill.frame->fill.set_behavior(0.12f, ui::Easing::kInOutQuad);
        pill.label = pill.frame->add<ui::TextNode>();
        pill.label->set_font_cache(&fonts);
        pill.label->set_text(view_label(view));
        pill.label->pixel_size = 18.0f;
        pill.label->anchors = ui::Anchors::Fill();
        pill.label->h_align = ui::HAlign::kCenter;
        pill.label->v_align = ui::VAlign::kCenter;
        pill.label->color.set_behavior(0.12f, ui::Easing::kInOutQuad);
        pills_.push_back(pill);
    }
    for (Pill& pill : pills_) {
        const View view = pill.view;
        pill.frame->on_hover_changed = [this](bool) { refresh(); };
        pill.frame->on_click = [this, view] { set_view(view); };
    }
    refresh();
}

bool ViewSwitch::available(View view) const {
    switch (view) {
        case View::kFootage: return true;
        case View::kSubject: return matte_ && subject_;
        default: return matte_;
    }
}

void ViewSwitch::set_available(bool matte, bool subject) {
    matte_ = matte;
    subject_ = subject;
    if (!available(view_)) set_view(View::kFootage);
    refresh();
}

void ViewSwitch::set_view(View view) {
    if (view == view_ || !available(view)) return;
    view_ = view;
    refresh();
    if (on_view_changed) on_view_changed(view_);
}

void ViewSwitch::refresh() {
    for (Pill& pill : pills_) {
        const bool active = pill.view == view_;
        const bool usable = available(pill.view);
        pill.frame->fill = active ? theme::kWhite : theme::kClear;
        pill.frame->border_color = !usable                ? theme::kDormant
                                   : active               ? theme::kWhite
                                   : pill.frame->hovered() ? theme::kHover
                                                          : theme::kWhite;
        pill.label->color = active ? theme::kActiveIcon : usable ? theme::kWhite : theme::kDormant;
    }
}

void ViewSwitch::layout_self() {
    const float pill_w = std::max(0.0f, (width - kGap) * 0.5f);
    for (std::size_t i = 0; i < pills_.size(); ++i) {
        ui::RectNode* f = pills_[i].frame;
        f->x = static_cast<float>(i % 2) * (pill_w + kGap);
        f->y = static_cast<float>(i / 2) * (kPillHeight + kGap);
        f->width = pill_w;
        f->height = kPillHeight;
    }
}

}  // namespace ps::editor

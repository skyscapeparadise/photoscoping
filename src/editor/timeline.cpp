#include "editor/timeline.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>

#include "editor/theme.hpp"
#include "editor/timecode.hpp"

namespace ps::editor {

namespace {

constexpr float kTimecodeWidth = 200.0f;
constexpr int kTransportCount = 5;
constexpr float kButton = 36.0f;
constexpr float kButtonGap = 8.0f;
constexpr float kTransportInset = 14.0f;
constexpr float kTransportWidth =
    kTransportInset * 2.0f + kTransportCount * kButton + (kTransportCount - 1) * kButtonGap;
// Narrower than this and the clock gives up its place to the ruler, which is
// the one part of the strip that is no use at all when it is small.
constexpr float kMinRuler = 160.0f;

constexpr float kMinPixelsPerSecond = 4.0f;
// Enough to give each frame of 24fps footage two dozen pixels.
constexpr float kMaxPixelsPerSecond = 2400.0f;
// A fitted clip leaves a little ruler past its end, so the last frame is not
// jammed against the clock.
constexpr float kFitFraction = 0.92f;
// Labels need about this much room each, at 10px.
constexpr float kLabelSpacing = 90.0f;
// Below this a tick per frame is a smear rather than a scale.
constexpr float kMinFrameSpacing = 8.0f;

// Label steps in seconds, each with how many divisions it gets.
struct Step {
    int seconds;
    int divisions;
};
constexpr Step kSteps[] = {{1, 10},  {2, 4},   {5, 5},   {10, 10}, {15, 3},
                           {30, 6},  {60, 6},  {120, 4}, {300, 5}, {600, 10}};

std::string label_for(int seconds) {
    char out[16];
    if (seconds >= 3600)
        std::snprintf(out, sizeof out, "%d:%02d:%02d", seconds / 3600, seconds / 60 % 60,
                      seconds % 60);
    else
        std::snprintf(out, sizeof out, "%d:%02d", seconds / 60, seconds % 60);
    return out;
}

}  // namespace

void Timeline::build(ui::FontCache& fonts, const gpu::IconAtlas& atlas) {
    fonts_ = &fonts;

    ruler_ = add<ui::Node>();
    ruler_->clip = true;
    auto* ruler_bg = ruler_->add<ui::RectNode>();
    ruler_bg->anchors = ui::Anchors::Fill();
    ruler_bg->fill = theme::kBar;

    ticks_ = ruler_->add<ui::Node>();
    ticks_->height = kHeight;

    ruler_rule_ = ruler_->add<ui::RectNode>();
    ruler_rule_->height = 1.0f;
    ruler_rule_->fill = theme::kBarRule;

    playhead_ = ruler_->add<ui::RectNode>();
    playhead_->height = kHeight;
    playhead_tip_ = playhead_->add<ui::IconNode>();
    playhead_tip_->atlas = &atlas;
    playhead_tip_->icon_name = "triangledown";
    playhead_tip_->colorize = true;

    // Unlike understory's ruler, which only moves when its head is grabbed,
    // pressing anywhere here seeks: scrubbing is most of what this ruler is
    // for.
    ruler_->on_pointer_moved = [this](float px, float) {
        pointer_x_ = px - ruler_->abs_x();
        const bool near = near_playhead(pointer_x_);
        if (near == playhead_hovered_) return;
        playhead_hovered_ = near;
        refresh_playhead_look();
    };
    ruler_->on_press = [this](float px, float) {
        if (frame_count_ <= 0) return;
        dragging_ = true;
        refresh_playhead_look();
        seek_to(px - ruler_->abs_x());
    };
    ruler_->on_drag = [this](float px, float) {
        if (dragging_) seek_to(px - ruler_->abs_x());
    };
    ruler_->on_release = [this] {
        dragging_ = false;
        refresh_playhead_look();
    };
    ruler_->keeps_drag = true;
    // Ctrl zooms about the pointer, which is how a trackpad pinch arrives; a
    // bare wheel scrolls, sideways or not, since nothing above the strip
    // scrolls vertically to want it.
    ruler_->on_wheel = [this](float dx, float dy) {
        if ((SDL_GetModState() & SDL_KMOD_CTRL) != 0) {
            const float before = pixels_per_second_;
            const float after = std::clamp(before * std::exp(dy / 480.0f), kMinPixelsPerSecond,
                                           kMaxPixelsPerSecond);
            if (after == before) return true;
            const float at = (pointer_x_ + scroll_) / before;
            pixels_per_second_ = after;
            scroll_ = std::max(0.0f, at * after - pointer_x_);
        } else {
            scroll_ -= dx != 0.0f ? dx : dy;
        }
        refresh_playhead();
        return true;
    };

    timecode_ = add<ui::Node>();
    auto* tc_bg = timecode_->add<ui::RectNode>();
    tc_bg->anchors = ui::Anchors::Fill();
    tc_bg->fill = theme::kBar;
    timecode_rule_ = timecode_->add<ui::RectNode>();
    timecode_rule_->height = 1.0f;
    timecode_rule_->fill = theme::kBarRule;
    auto* tc_left = timecode_->add<ui::RectNode>();
    tc_left->width = 1.0f;
    tc_left->height = kHeight;
    tc_left->fill = theme::kBarRule;
    // Eight digits at 20 and three separators at 10 is 190, centred.
    float cell_x = (kTimecodeWidth - 190.0f) * 0.5f + 1.0f;
    for (int i = 0; i < kTimecodeCells; ++i) {
        const bool separator = (i == 2 || i == 5 || i == 8);
        auto* cell = timecode_->add<ui::TextNode>();
        cell->set_font_cache(&fonts);
        cell->set_text(separator ? ":" : "0");
        cell->pixel_size = 34.0f;
        cell->color = theme::kWhite;
        cell->x = cell_x;
        cell->y = (kHeight - 42.0f) * 0.5f;
        cell->width = separator ? 10.0f : 20.0f;
        cell->height = 42.0f;
        cell->h_align = ui::HAlign::kCenter;
        cell->v_align = ui::VAlign::kCenter;
        timecode_cells_[i] = cell;
        cell_x += cell->width;
    }

    transport_ = add<ui::Node>();
    auto* tr_bg = transport_->add<ui::RectNode>();
    tr_bg->anchors = ui::Anchors::Fill();
    tr_bg->fill = theme::kBar;
    transport_rule_ = transport_->add<ui::RectNode>();
    transport_rule_->height = 1.0f;
    transport_rule_->fill = theme::kBarRule;
    auto* tr_left = transport_->add<ui::RectNode>();
    tr_left->width = 1.0f;
    tr_left->height = kHeight;
    tr_left->fill = theme::kBarRule;
    static constexpr const char* kTransport[kTransportCount] = {"previous", "next", "stop",
                                                                "play", "pause"};
    for (int i = 0; i < kTransportCount; ++i) {
        auto* button = transport_->add<ui::RectNode>();
        button->x = kTransportInset + static_cast<float>(i) * (kButton + kButtonGap);
        button->y = (kHeight - kButton) * 0.5f;
        button->width = kButton;
        button->height = kButton;
        button->radius = 12.0f;
        button->border_width = 2.0f;
        button->border_color.set_behavior(0.08f, ui::Easing::kInOutQuad);
        button->fill.set_behavior(0.12f, ui::Easing::kInOutQuad);
        auto* icon = button->add<ui::IconNode>();
        icon->x = 7.0f;
        icon->y = 7.0f;
        icon->width = 22.0f;
        icon->height = 22.0f;
        icon->colorize = true;
        icon->atlas = &atlas;
        icon->icon_name = kTransport[i];
        icon->fill_mode = ui::IconNode::FillMode::kFit;
        icon->tint.set_behavior(0.12f, ui::Easing::kInOutQuad);
        transport_buttons_.push_back(button);
        transport_icons_.push_back(icon);
        button->on_hover_changed = [this](bool) { refresh_transport(); };

        const std::string name = kTransport[i];
        if (name == "previous") {
            button->on_click = [this] { if (on_step && frame_count_ > 0) on_step(-1); };
        } else if (name == "next") {
            button->on_click = [this] { if (on_step && frame_count_ > 0) on_step(+1); };
        } else if (name == "stop") {
            button->on_click = [this] { if (on_stop && frame_count_ > 0) on_stop(); };
        } else if (name == "play") {
            button->on_click = [this] { if (on_play && frame_count_ > 0) on_play(true); };
        } else if (name == "pause") {
            button->on_click = [this] { if (on_play && frame_count_ > 0) on_play(false); };
        }
    }

    refresh_timecode();
    refresh_transport();
    refresh_playhead_look();
}

void Timeline::set_clip(double fps, std::int64_t frame_count) {
    fps_ = fps > 0.0 ? fps : 24.0;
    frame_count_ = std::max<std::int64_t>(0, frame_count);
    frame_ = 0;
    scroll_ = 0.0f;
    fit_pending_ = frame_count_ > 0;
    refresh_timecode();
    refresh_transport();
    refresh_playhead();
}

void Timeline::set_frame(std::int64_t frame) {
    if (frame == frame_) return;
    frame_ = frame;
    refresh_timecode();
    refresh_transport();
    refresh_playhead();
}

void Timeline::set_playing(bool playing) {
    if (playing == playing_) return;
    playing_ = playing;
    refresh_transport();
}

void Timeline::set_matted(const std::vector<bool>& matted) {
    if (matted == matted_) return;
    matted_ = matted;
    built_width_ = -1.0f;  // the strip is part of the ruler
    refresh_playhead();
}

void Timeline::layout_self() {
    const bool show_clock = width - kTimecodeWidth - kTransportWidth >= kMinRuler;
    const float right = kTransportWidth + (show_clock ? kTimecodeWidth : 0.0f);

    ruler_->x = 0.0f;
    ruler_->y = 0.0f;
    ruler_->width = std::max(0.0f, width - right);
    ruler_->height = kHeight;
    ruler_rule_->width = ruler_->width;

    timecode_->visible = show_clock;
    timecode_->x = width - kTransportWidth - kTimecodeWidth;
    timecode_->width = kTimecodeWidth;
    timecode_->height = kHeight;
    timecode_rule_->width = kTimecodeWidth;

    transport_->x = width - kTransportWidth;
    transport_->width = kTransportWidth;
    transport_->height = kHeight;
    transport_rule_->width = kTransportWidth;

    // The whole clip across the ruler, once its width is known.
    if (fit_pending_ && ruler_->width > 0.0f) {
        fit_pending_ = false;
        const double duration = frame_count_ / fps_;
        if (duration > 0.0)
            pixels_per_second_ =
                std::clamp(static_cast<float>(ruler_->width * kFitFraction / duration),
                           kMinPixelsPerSecond, kMaxPixelsPerSecond);
        scroll_ = 0.0f;
    }
    refresh_playhead();
}

double Timeline::frame_seconds(std::int64_t frame) const { return frame / fps_; }

std::int64_t Timeline::frame_at(float ruler_x) const {
    const double seconds = (ruler_x + scroll_) / pixels_per_second_;
    const auto frame = static_cast<std::int64_t>(std::floor(seconds * fps_ + 0.5));
    return std::clamp<std::int64_t>(frame, 0, std::max<std::int64_t>(0, frame_count_ - 1));
}

void Timeline::seek_to(float ruler_x) {
    if (on_seek && frame_count_ > 0) on_seek(frame_at(ruler_x));
}

bool Timeline::near_playhead(float ruler_x) const {
    const float head = static_cast<float>(frame_seconds(frame_)) * pixels_per_second_ - scroll_;
    return frame_count_ > 0 && std::abs(ruler_x - head) <= 8.0f;
}

void Timeline::rebuild_ruler() {
    const float w = ruler_->width;
    if (w <= 0.0f || !fonts_) return;

    // The coarsest step whose labels still have room, and how finely it is
    // divided. Zoomed in far enough, the divisions become frames.
    const Step* step = &kSteps[std::size(kSteps) - 1];
    for (const Step& s : kSteps) {
        if (s.seconds * pixels_per_second_ >= kLabelSpacing) {
            step = &s;
            break;
        }
    }
    const int nominal = std::max(1, static_cast<int>(std::lround(fps_)));
    const bool frames = step->seconds == 1 && pixels_per_second_ / nominal >= kMinFrameSpacing;
    const int divisions = frames ? nominal : step->divisions;
    const double tick_seconds = static_cast<double>(step->seconds) / divisions;
    const double tick_px = tick_seconds * pixels_per_second_;

    const int first = std::max(0, static_cast<int>(std::floor(scroll_ / tick_px)));
    const int last = static_cast<int>(std::ceil((scroll_ + w) / tick_px));
    if (w == built_width_ && pixels_per_second_ == built_pps_ && first == built_first_ &&
        last == built_last_)
        return;
    built_width_ = w;
    built_pps_ = pixels_per_second_;
    built_first_ = first;
    built_last_ = last;

    // The span of the clip is rebuilt with the ticks; it is their background.
    ticks_->clear_children();
    clip_span_ = ticks_->add<ui::RectNode>();
    clip_span_->height = kHeight;
    clip_span_->fill = theme::kClipSpan;
    clip_span_->width = static_cast<float>(frame_seconds(frame_count_)) * pixels_per_second_;
    // One mark per unbroken run of matted frames, rather than one per frame.
    const auto runs = static_cast<std::int64_t>(std::min<std::size_t>(
        matted_.size(), static_cast<std::size_t>(std::max<std::int64_t>(0, frame_count_))));
    for (std::int64_t i = 0; i < runs;) {
        if (!matted_[static_cast<std::size_t>(i)]) { ++i; continue; }
        std::int64_t end = i;
        while (end < runs && matted_[static_cast<std::size_t>(end)]) ++end;
        auto* mark = ticks_->add<ui::RectNode>();
        mark->x = static_cast<float>(frame_seconds(i)) * pixels_per_second_;
        mark->y = 1.0f;
        mark->height = 3.0f;
        mark->fill = theme::kHover;
        mark->width = static_cast<float>(frame_seconds(end - i)) * pixels_per_second_;
        i = end;
    }
    ticks_->width = w;

    for (int i = first; i <= last; ++i) {
        const float x = static_cast<float>(i * tick_px);
        const bool major = i % divisions == 0;
        const bool medium = !major && divisions % 2 == 0 && i % (divisions / 2) == 0;
        auto* tick = ticks_->add<ui::RectNode>();
        tick->x = x;
        tick->width = major ? 1.5f : 1.0f;
        tick->height = major ? 12.0f : (medium ? 7.0f : 4.0f);
        tick->y = kHeight - tick->height;
        tick->fill = gpu::Color::hex(major ? 0x777777 : (medium ? 0x555555 : 0x444444));

        if (!major) continue;
        auto* label = ticks_->add<ui::TextNode>();
        label->set_font_cache(fonts_);
        label->set_text(label_for(i / divisions * step->seconds));
        label->pixel_size = 10.0f;
        label->color = gpu::Color::hex(0x999999);
        label->x = x + 3.0f;
        label->y = kHeight - 28.0f;
        label->width = 48.0f;
        label->height = 14.0f;
    }
}

// The head sits where the decoder is, less the scroll. While playing it
// pushes the ruler along once it reaches the middle, as understory's does;
// stepped or sought off either end while paused, the ruler jumps to bring it
// back rather than leaving it out of sight.
void Timeline::refresh_playhead() {
    if (!playhead_ || !ruler_) return;
    const float w = ruler_->width;
    const float head_px = static_cast<float>(frame_seconds(frame_)) * pixels_per_second_;
    if (!dragging_ && w > 0.0f) {
        if (playing_ && head_px > scroll_ + w * 0.5f) scroll_ = head_px - w * 0.5f;
        if (!playing_ && (head_px < scroll_ || head_px > scroll_ + w))
            scroll_ = head_px - w * 0.5f;
    }
    // Never so far that the clip's end leaves the middle of the ruler.
    const float end_px = static_cast<float>(frame_seconds(frame_count_)) * pixels_per_second_;
    scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, end_px - w * 0.5f));

    playhead_->visible = frame_count_ > 0;
    playhead_->x = head_px - scroll_;
    ticks_->x = -scroll_;
    rebuild_ruler();
}

void Timeline::refresh_playhead_look() {
    const bool active = dragging_ || playhead_hovered_;
    const gpu::Color colour = active ? theme::kPlayheadLit : theme::kPlayhead;
    playhead_->width = active ? 2.5f : 2.0f;
    playhead_->fill = colour;
    const float half = active ? 8.0f : 6.0f;
    playhead_tip_->tint = colour;
    playhead_tip_->x = -half + playhead_->width * 0.5f;
    playhead_tip_->y = 0.0f;
    playhead_tip_->width = half * 2.0f;
    playhead_tip_->height = half + 4.0f;
}

void Timeline::refresh_timecode() {
    const std::string tc = format_timecode(frame_, fps_);
    for (int i = 0; i < kTimecodeCells; ++i)
        timecode_cells_[i]->set_text(i < static_cast<int>(tc.size()) ? std::string(1, tc[i])
                                                                       : std::string());
}

void Timeline::refresh_transport() {
    const bool have_clip = frame_count_ > 0;
    for (std::size_t i = 0; i < transport_icons_.size(); ++i) {
        bool active = false;
        if (i == 3) active = playing_;
        else if (i == 4) active = !playing_ && frame_ > 0;
        active = active && have_clip;
        transport_icons_[i]->tint =
            active ? theme::kActiveIcon : (have_clip ? theme::kWhite : theme::kDormant);
        transport_buttons_[i]->fill = active ? theme::kWhite : theme::kClear;
        transport_buttons_[i]->border_color =
            !have_clip                         ? theme::kDormant
            : transport_buttons_[i]->hovered() ? theme::kHover
                                               : theme::kWhite;
    }
}

}  // namespace ps::editor

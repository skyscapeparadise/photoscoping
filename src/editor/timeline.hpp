#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "gpu/icon_atlas.hpp"
#include "ui/font.hpp"
#include "ui/node.hpp"
#include "ui/text.hpp"
#include "ui/widgets.hpp"

namespace ps::editor {

// The strip along the bottom of the window: the ruler with its playhead, the
// timecode, and the transport. Understory's bottom bar without the chapters,
// counting the clip's frames rather than a story's seconds.
//
// It shows where the decoder is and reports what the user asked for; it never
// moves the clip itself. Whoever owns the decoder answers on_seek and the
// transport callbacks and then reports back through set_frame and set_playing,
// so the head only ever shows a frame that was actually decoded.
class Timeline : public ui::Node {
public:
    static constexpr float kHeight = 50.0f;

    void build(ui::FontCache& fonts, const gpu::IconAtlas& atlas);

    // A clip of `frame_count` frames at `fps`; zero frames for none. Fits the
    // whole clip to the ruler the next time it is laid out.
    void set_clip(double fps, std::int64_t frame_count);
    void set_frame(std::int64_t frame);
    void set_playing(bool playing);
    // Which of the clip's frames have a matte, one entry per frame. Marked
    // along the top of the ruler, so a run in progress fills it in.
    void set_matted(const std::vector<bool>& matted);
    // The frames hand-made fixes have keys on: marked under the matte line.
    void set_keys(const std::vector<std::int64_t>& frames);

    std::function<void(std::int64_t)> on_seek;
    std::function<void(bool)> on_play;   // true to play, false to pause
    std::function<void()> on_stop;
    std::function<void(int)> on_step;    // -1 or +1 frames

protected:
    void layout_self() override;

private:
    double frame_seconds(std::int64_t frame) const;
    std::int64_t frame_at(float ruler_x) const;
    void seek_to(float ruler_x);
    void rebuild_ruler();
    void refresh_playhead();
    void refresh_playhead_look();
    void refresh_timecode();
    void refresh_transport();
    bool near_playhead(float ruler_x) const;

    ui::FontCache* fonts_ = nullptr;

    double fps_ = 24.0;
    std::int64_t frame_count_ = 0;
    std::int64_t frame_ = 0;
    std::vector<bool> matted_;
    std::vector<std::int64_t> keys_;
    bool playing_ = false;
    bool fit_pending_ = false;

    // The ruler.
    ui::Node* ruler_ = nullptr;
    ui::RectNode* ruler_rule_ = nullptr;
    // Scrolled by moving it: its x is minus the scroll, so it must not be
    // anchored, or layout would put it back to zero.
    ui::Node* ticks_ = nullptr;
    ui::RectNode* clip_span_ = nullptr;  // rebuilt with the ticks
    ui::RectNode* playhead_ = nullptr;
    ui::IconNode* playhead_tip_ = nullptr;
    float scroll_ = 0.0f;
    float pixels_per_second_ = 60.0f;
    float pointer_x_ = 0.0f;
    bool dragging_ = false;
    bool playhead_hovered_ = false;
    // What the ticks were last built for, so they are rebuilt only when any
    // of it moves.
    float built_width_ = -1.0f;
    float built_pps_ = 0.0f;
    int built_first_ = 0;
    int built_last_ = -1;

    // The clock: one cell per character, so a digit never shifts as the
    // reading changes -- a clock whose colons slide about as it runs is hard
    // to read at a glance.
    static constexpr int kTimecodeCells = 11;
    ui::Node* timecode_ = nullptr;
    ui::RectNode* timecode_rule_ = nullptr;
    ui::TextNode* timecode_cells_[kTimecodeCells] = {};

    ui::Node* transport_ = nullptr;
    ui::RectNode* transport_rule_ = nullptr;
    std::vector<ui::RectNode*> transport_buttons_;
    std::vector<ui::IconNode*> transport_icons_;
};

}  // namespace ps::editor

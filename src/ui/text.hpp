#pragma once

#include <string>

#include "ui/font.hpp"
#include "ui/node.hpp"

namespace ps::ui {

// Where a line that does not fit is shortened.
enum class Elide { kNone, kLeft, kMiddle, kRight };

// Word wrapping, or none.
enum class Wrap { kNone, kWord };

enum class HAlign { kLeft, kCenter, kRight };
enum class VAlign { kTop, kCenter, kBottom };

// A run of text.
//
// Elide is implemented here rather than by SDL3_ttf, which has no notion of it.
// TTF_MeasureString answers "how much of this fits in N pixels", which is
// exactly the query eliding needs, so no binary search is involved.
class TextNode : public Node {
public:
    ~TextNode() override;

    void set_font_cache(FontCache* cache) { fonts_ = cache; }

    void set_text(const std::string& value);
    const std::string& text() const { return text_; }

    float pixel_size = 14.0f;   // logical pixels
    bool bold = false;
    Animated<Color> color{Color{1, 1, 1, 1}};
    Elide elide = Elide::kNone;
    Wrap wrap = Wrap::kNone;
    HAlign h_align = HAlign::kLeft;
    VAlign v_align = VAlign::kTop;

    // Natural size of the text as last laid out, in logical pixels -- what a
    // caller sizing a node to its content needs.
    float content_width() const { return content_w_; }
    float content_height() const { return content_h_; }

protected:
    void paint_self(gpu::DrawList& draw, const PaintContext& ctx) override;
    bool tick_self(float dt) override;

private:
    // Applies elide/wrap for the current width and pushes the result into the
    // cached TTF_Text. Only re-runs when something it depends on changed --
    // reshaping every frame would undo the point of the engine's caching.
    void rebuild(gpu::DrawList& draw);
    std::string elided_for(TTF_Font* font, float max_device_width) const;

    FontCache* fonts_ = nullptr;
    TTF_Text* ttf_text_ = nullptr;
    TTF_Font* font_ = nullptr;

    std::string text_;
    std::string shaped_source_;   // what ttf_text_ currently holds
    float shaped_scale_ = 0.0f;
    float shaped_width_ = -1.0f;
    float shaped_pixel_size_ = -1.0f;
    bool shaped_bold_ = false;

    float content_w_ = 0.0f;
    float content_h_ = 0.0f;
};

}  // namespace ps::ui

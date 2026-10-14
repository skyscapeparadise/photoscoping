#include "ui/text.hpp"

#include <algorithm>
#include <cmath>

namespace ps::ui {

namespace {

constexpr const char* kEllipsis = "\xE2\x80\xA6";  // U+2026

int measure_width(TTF_Font* font, const char* s, std::size_t len) {
    int w = 0, h = 0;
    if (!TTF_GetStringSize(font, s, len, &w, &h)) return 0;
    return w;
}

// Longest prefix of `s` that fits `max_width`, in bytes.
std::size_t prefix_fitting(TTF_Font* font, const std::string& s, int max_width) {
    int measured_w = 0;
    std::size_t measured_len = 0;
    if (!TTF_MeasureString(font, s.c_str(), s.size(), max_width, &measured_w, &measured_len)) {
        return 0;
    }
    return measured_len;
}

// Walks back from the end one UTF-8 codepoint at a time until the suffix fits.
// There is no TTF_MeasureString equivalent that measures from the right, and
// left eliding is rare enough that a linear walk is not worth optimising.
std::size_t suffix_fitting(TTF_Font* font, const std::string& s, int max_width) {
    std::size_t start = s.size();
    while (start > 0) {
        std::size_t next = start - 1;
        while (next > 0 && (static_cast<unsigned char>(s[next]) & 0xC0) == 0x80) --next;
        if (measure_width(font, s.c_str() + next, s.size() - next) > max_width) break;
        start = next;
    }
    return start;
}

}  // namespace

TextNode::~TextNode() {
    if (ttf_text_) TTF_DestroyText(ttf_text_);
}

void TextNode::set_text(const std::string& value) { text_ = value; }

bool TextNode::tick_self(float dt) { return color.tick(dt); }

std::string TextNode::elided_for(TTF_Font* font, float max_device_width) const {
    const int max_w = static_cast<int>(max_device_width);
    if (elide == Elide::kNone || max_w <= 0) return text_;
    if (measure_width(font, text_.c_str(), text_.size()) <= max_w) return text_;

    const int ell_w = measure_width(font, kEllipsis, 3);
    const int budget = max_w - ell_w;
    if (budget <= 0) return kEllipsis;

    switch (elide) {
        case Elide::kRight:
            return text_.substr(0, prefix_fitting(font, text_, budget)) + kEllipsis;
        case Elide::kLeft:
            return std::string(kEllipsis) + text_.substr(suffix_fitting(font, text_, budget));
        case Elide::kMiddle: {
            const int half = budget / 2;
            const std::size_t head = prefix_fitting(font, text_, half);
            const std::size_t tail = suffix_fitting(font, text_, budget - half);
            // Overlapping head and tail would repeat characters.
            if (head >= tail) return text_.substr(0, head) + kEllipsis;
            return text_.substr(0, head) + kEllipsis + text_.substr(tail);
        }
        default:
            return text_;
    }
}

void TextNode::rebuild(gpu::DrawList& draw) {
    const float scale = draw.scale();
    const float device_size = pixel_size * scale;
    const float device_width = width * scale;

    const bool dirty = shaped_source_ != text_ || shaped_scale_ != scale ||
                       shaped_width_ != device_width ||
                       shaped_pixel_size_ != pixel_size || shaped_bold_ != bold ||
                       ttf_text_ == nullptr;
    if (!dirty) return;

    font_ = fonts_->font(device_size, bold);
    if (!font_) return;

    const std::string display = elided_for(font_, device_width);

    if (!ttf_text_) {
        ttf_text_ = TTF_CreateText(fonts_->engine(), font_, display.c_str(), display.size());
    } else {
        TTF_SetTextFont(ttf_text_, font_);
        TTF_SetTextString(ttf_text_, display.c_str(), display.size());
    }
    if (!ttf_text_) return;

    // Wrapping and eliding are mutually exclusive: a wrapped block has no
    // single line to trim.
    TTF_SetTextWrapWidth(ttf_text_,
                         wrap == Wrap::kWord ? static_cast<int>(device_width) : 0);

    int w = 0, h = 0;
    TTF_GetTextSize(ttf_text_, &w, &h);
    content_w_ = static_cast<float>(w) / scale;
    content_h_ = static_cast<float>(h) / scale;

    shaped_source_ = text_;
    shaped_scale_ = scale;
    shaped_width_ = device_width;
    shaped_pixel_size_ = pixel_size;
    shaped_bold_ = bold;
}

void TextNode::paint_self(gpu::DrawList& draw, const PaintContext& ctx) {
    if (!fonts_ || !fonts_->ready() || text_.empty()) return;

    rebuild(draw);
    if (!ttf_text_) return;

    // Alignment positions the shaped block inside the node's box. Done here
    // rather than in layout because it depends on the measured content size.
    float ox = ctx.origin_x;
    float oy = ctx.origin_y;
    if (h_align == HAlign::kCenter)      ox += (width - content_w_) * 0.5f;
    else if (h_align == HAlign::kRight)  ox += width - content_w_;
    if (v_align == VAlign::kCenter)      oy += (height - content_h_) * 0.5f;
    else if (v_align == VAlign::kBottom) oy += height - content_h_;

    Color c = color.value();
    c.a *= ctx.opacity;
    if (c.a <= 0.0f) return;

    for (TTF_GPUAtlasDrawSequence* seq = TTF_GetGPUTextDrawData(ttf_text_); seq;
         seq = seq->next) {
        // ALPHA means the colour channels are white and the glyph is pure
        // coverage, so it tints. COLOR means the atlas holds real image data
        // (colour emoji), which must modulate instead or the emoji comes out a
        // flat silhouette in the text colour.
        const bool colorize = (seq->image_type != TTF_IMAGE_COLOR);
        // No block-height offset. The engine's vertices already run from 0 at
        // the top of the block down to -height, so inverting Y in the draw list
        // lands them at [oy, oy + height] on its own. Measured against a known
        // box: adding the height again puts the text one whole line too low,
        // which reads as "vertical centring is broken" rather than as an
        // offset bug.
        draw.add_text_geometry(seq->atlas_texture, seq->xy, seq->uv, seq->indices,
                               seq->num_indices, ox, oy, c, colorize);
    }
}

}  // namespace ps::ui

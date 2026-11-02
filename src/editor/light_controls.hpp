#pragma once

#include <SDL3/SDL.h>

#include <functional>
#include <string>

#include "fixes/fixes.hpp"
#include "ui/font.hpp"
#include "ui/node.hpp"
#include "ui/text.hpp"
#include "ui/widgets.hpp"

namespace ps::editor {

// A labelled slider in the card: name on the left, value on the right, a
// track and knob between. Drag anywhere on the row to set it; double-click to
// put it back to its default.
class Slider : public ui::Node {
public:
    void build(ui::FontCache& fonts, const char* label);
    void set_range(float lo, float hi, float initial) {
        lo_ = lo;
        hi_ = hi;
        default_ = initial;
    }
    void set_value(float v);
    float value() const { return value_; }

    // While dragging (`done` false) and once let go (`done` true).
    std::function<void(float value, bool done)> on_change;
    // Before the first change of a drag: for undo.
    std::function<void()> on_begin;

protected:
    void layout_self() override;

private:
    void set_from(float x, bool done);

    ui::TextNode* label_ = nullptr;
    ui::TextNode* readout_ = nullptr;
    ui::RectNode* track_ = nullptr;
    ui::RectNode* fill_ = nullptr;
    ui::RectNode* knob_ = nullptr;
    float lo_ = 0.0f, hi_ = 1.0f, default_ = 0.0f, value_ = 0.0f;
    bool dragging_ = false;
};

// The selected light map crop's adjustment, Photoshop's Hue/Saturation with
// its Colorize: shown in the card in place of the clip's rows while the
// light tool is in hand.
class LightControls : public ui::Node {
public:
    void build(ui::FontCache& fonts);
    // The crop being adjusted, or null for none (the controls grey out);
    // and its exported size, for the line under them.
    void bind(const fixes::LightCrop* crop, const std::string& size_text);
    // The crop as it will export, under the controls: a texture of `aspect`
    // (width over height), or null while there is none to show.
    void set_preview(SDL_GPUTexture* texture, float aspect, const std::string& caption);

    // Before an edit, for undo; while it changes; when it is done.
    std::function<void()> on_edit_begin;
    std::function<void(const fixes::LightCrop& adjusted, bool done)> on_adjust;

protected:
    void layout_self() override;

private:
    void apply(bool done);
    void refresh_toggle();

    Slider* hue_ = nullptr;
    Slider* saturation_ = nullptr;
    Slider* lightness_ = nullptr;
    ui::RectNode* colorize_ = nullptr;
    ui::TextNode* colorize_label_ = nullptr;
    ui::TextNode* size_ = nullptr;
    ui::TextNode* hint_ = nullptr;
    ui::RectNode* preview_frame_ = nullptr;
    ui::IconNode* preview_ = nullptr;
    ui::TextNode* caption_ = nullptr;
    float preview_aspect_ = 1.0f;
    fixes::LightCrop crop_;
    std::string size_text_cache_;
    bool bound_ = false;
};

}  // namespace ps::editor

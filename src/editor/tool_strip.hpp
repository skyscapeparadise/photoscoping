#pragma once

#include <functional>
#include <string>
#include <vector>

#include "gpu/icon_atlas.hpp"
#include "ui/font.hpp"
#include "ui/node.hpp"
#include "ui/text.hpp"
#include "ui/widgets.hpp"

namespace ps::editor {

// Which tool the viewer is in.
//
//   select    nothing yet beyond pointing
//   navigate  drag to pan (zoom is everywhere)
//   garbage   draw and keyframe garbage mattes
//   rematte   a box the stack re-runs in at more detail (not built yet)
//   paint     paint coverage out of the matte and back in (not built yet)
enum class Tool { kSelect, kNavigate, kGarbage, kRematte, kPaint };

// Lowercase, as the inspector shows it.
const char* tool_label(Tool tool);

// The row of tool buttons at the top of the inspector.
//
// Understory's strip is a fixed 5x2 grid of 72px buttons. This one is a single
// row whose buttons size themselves from the width it is given, between a floor
// that keeps the hands legible and a ceiling past which they stop growing and
// the gaps take up the slack instead.
//
// A tool without its hand drawn yet shows its name instead, and one not built
// yet is there, greyed, and cannot be chosen.
class ToolStrip : public ui::Node {
public:
    void build(const gpu::IconAtlas& atlas, ui::FontCache& fonts);

    void set_tool(Tool tool);
    Tool tool() const { return tool_; }

    // The strip's height at a given width: one row of square buttons.
    static float height_for(float width);

    std::function<void(Tool)> on_tool_changed;
    // Preview is an action rather than a tool: it hands the whole window to
    // the footage and never stays selected.
    std::function<void()> on_preview;

protected:
    void layout_self() override;

private:
    struct Button {
        ui::Node* root = nullptr;
        ui::RectNode* frame = nullptr;
        ui::IconNode* icon = nullptr;
        ui::TextNode* label = nullptr;
        bool is_preview = false;
        bool built = true;
        Tool tool = Tool::kSelect;
    };

    void refresh_states();

    std::vector<Button> buttons_;
    Tool tool_ = Tool::kSelect;
};

}  // namespace ps::editor

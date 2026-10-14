#pragma once

#include <string>
#include <utility>
#include <vector>

#include "editor/tool_strip.hpp"
#include "gpu/icon_atlas.hpp"
#include "ui/font.hpp"
#include "ui/node.hpp"
#include "ui/text.hpp"
#include "ui/widgets.hpp"

namespace ps::editor {

// What the inspector says about the open clip.
struct ClipInfo {
    std::string name;
    // Label and value, in the order shown.
    std::vector<std::pair<std::string, std::string>> rows;
};

// The panel to the right of the viewer: the tool strip above a card, in the
// shape of understory's inspector. Draws no background of its own -- the
// nebula is a GPU pass underneath it, so everything here is outline and type
// over that.
//
// Laid out from its own width, so it reflows as the panel is dragged wider or
// narrower: the buttons grow and shrink, the clip's name scales with the card,
// and rows that no longer fit below it are left out rather than squeezed.
class Inspector : public ui::Node {
public:
    void build(ui::FontCache& fonts, const gpu::IconAtlas& atlas);

    ToolStrip* tools() const { return tools_; }
    void set_clip(const ClipInfo& info);

protected:
    void layout_self() override;

private:
    struct Row {
        ui::TextNode* label = nullptr;
        ui::TextNode* value = nullptr;
    };

    void rebuild_rows();

    ui::FontCache* fonts_ = nullptr;
    ToolStrip* tools_ = nullptr;
    ui::RectNode* card_ = nullptr;
    ui::TextNode* heading_ = nullptr;
    ui::TextNode* title_ = nullptr;
    ui::RectNode* rule_ = nullptr;
    ui::TextNode* caption_ = nullptr;
    ui::Node* rows_ = nullptr;
    std::vector<Row> row_nodes_;
    ClipInfo info_;
};

}  // namespace ps::editor

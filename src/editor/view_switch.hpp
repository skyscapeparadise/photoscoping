#pragma once

#include <functional>
#include <vector>

#include "ui/font.hpp"
#include "ui/node.hpp"
#include "ui/text.hpp"
#include "ui/widgets.hpp"

namespace ps::editor {

// How the viewer shows the frame.
enum class View { kFootage, kMatte, kSubject, kOverlay };

const char* view_label(View view);

// Four pills in a 2x2 grid at the foot of the inspector, in the shape of the
// buttons under understory's: outlined, and filled white for the one in use.
//
// The three that show a matte are dormant until there is one to show.
class ViewSwitch : public ui::Node {
public:
    static constexpr float kPillHeight = 36.0f;
    static constexpr float kGap = 8.0f;
    static constexpr float kHeight = kPillHeight * 2.0f + kGap;

    void build(ui::FontCache& fonts);

    void set_view(View view);
    View view() const { return view_; }
    // Whether a matte (and a subject) exist to be shown.
    void set_available(bool matte, bool subject);
    bool available(View view) const;

    std::function<void(View)> on_view_changed;

protected:
    void layout_self() override;

private:
    struct Pill {
        View view = View::kFootage;
        ui::RectNode* frame = nullptr;
        ui::TextNode* label = nullptr;
    };

    void refresh();

    std::vector<Pill> pills_;
    View view_ = View::kFootage;
    bool matte_ = false;
    bool subject_ = false;
};

}  // namespace ps::editor

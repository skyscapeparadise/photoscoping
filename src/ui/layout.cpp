#include "ui/layout.hpp"

#include <algorithm>
#include <vector>

namespace ps::ui {

void LinearLayout::layout_self() {
    const auto& kids = children();
    implicit_width = 0.0f;
    implicit_height = 0.0f;
    if (kids.empty()) return;

    const bool vertical = (direction_ == Direction::kVertical);
    const float main_extent = vertical ? height : width;
    const float cross_extent = vertical ? width : height;

    std::size_t visible_count = 0;
    for (const auto& c : kids) {
        if (c->visible) ++visible_count;
    }
    if (visible_count == 0) return;

    const float total_spacing = spacing * static_cast<float>(visible_count - 1);

    // First pass: how much the non-stretching children want.
    float fixed = 0.0f;
    int fill_count = 0;
    for (const auto& c : kids) {
        if (!c->visible) continue;
        const bool fills = vertical ? c->layout_hints.fill_height : c->layout_hints.fill_width;
        const float preferred = vertical ? c->layout_hints.preferred_height
                                         : c->layout_hints.preferred_width;
        if (fills && preferred < 0.0f) {
            ++fill_count;
        } else {
            fixed += (preferred >= 0.0f) ? preferred : (vertical ? c->height : c->width);
        }
    }

    const float leftover = std::max(0.0f, main_extent - total_spacing - fixed);
    const float per_fill = (fill_count > 0) ? leftover / static_cast<float>(fill_count) : 0.0f;

    float pen = 0.0f;
    for (const auto& c : kids) {
        if (!c->visible) continue;

        const bool fills_main = vertical ? c->layout_hints.fill_height
                                         : c->layout_hints.fill_width;
        const float preferred_main = vertical ? c->layout_hints.preferred_height
                                              : c->layout_hints.preferred_width;
        const float main_size = (fills_main && preferred_main < 0.0f)
                                    ? per_fill
                                    : (preferred_main >= 0.0f
                                           ? preferred_main
                                           : (vertical ? c->height : c->width));

        const bool fills_cross = vertical ? c->layout_hints.fill_width
                                          : c->layout_hints.fill_height;
        const float preferred_cross = vertical ? c->layout_hints.preferred_width
                                               : c->layout_hints.preferred_height;
        float cross_size;
        if (preferred_cross >= 0.0f) {
            cross_size = preferred_cross;
        } else if (fills_cross || stretch_cross_axis) {
            cross_size = cross_extent;
        } else {
            cross_size = vertical ? c->width : c->height;
        }

        if (vertical) {
            c->x = 0.0f;
            c->y = pen;
            c->width = cross_size;
            c->height = main_size;
        } else {
            c->x = pen;
            c->y = 0.0f;
            c->width = main_size;
            c->height = cross_size;
        }
        pen += main_size + spacing;
    }

    // What the children actually needed, which is not the extent they were
    // given: a column told to fill a 320px parent still reports 1100 here if
    // that is how far its rows run. Trailing spacing is not part of it.
    const float main_total = std::max(0.0f, pen - spacing);
    float cross_total = 0.0f;
    for (const auto& c : kids) {
        if (!c->visible) continue;
        cross_total = std::max(cross_total, vertical ? c->width : c->height);
    }
    implicit_height = vertical ? main_total : cross_total;
    implicit_width = vertical ? cross_total : main_total;
}

void GridLayout::layout_self() {
    implicit_width = 0.0f;
    implicit_height = 0.0f;

    const int cols = std::max(1, columns);
    std::vector<Node*> cells;
    for (const auto& c : children()) {
        if (c->visible) cells.push_back(c.get());
    }
    if (cells.empty()) return;

    const int rows = static_cast<int>((cells.size() + cols - 1) / cols);

    // A child's preferred size wins over whatever it currently happens to be,
    // matching how the linear layouts read the same hints.
    auto want_w = [](const Node* n) {
        return n->layout_hints.preferred_width >= 0.0f ? n->layout_hints.preferred_width
                                                       : n->width;
    };
    auto want_h = [](const Node* n) {
        return n->layout_hints.preferred_height >= 0.0f ? n->layout_hints.preferred_height
                                                        : n->height;
    };

    std::vector<float> col_w(static_cast<std::size_t>(cols), 0.0f);
    std::vector<float> row_h(static_cast<std::size_t>(rows), 0.0f);
    for (std::size_t i = 0; i < cells.size(); ++i) {
        const std::size_t c = i % static_cast<std::size_t>(cols);
        const std::size_t r = i / static_cast<std::size_t>(cols);
        col_w[c] = std::max(col_w[c], want_w(cells[i]));
        row_h[r] = std::max(row_h[r], want_h(cells[i]));
    }

    std::vector<float> col_x(static_cast<std::size_t>(cols), 0.0f);
    for (int c = 1; c < cols; ++c) {
        col_x[c] = col_x[c - 1] + col_w[c - 1] + column_spacing;
    }
    std::vector<float> row_y(static_cast<std::size_t>(rows), 0.0f);
    for (int r = 1; r < rows; ++r) {
        row_y[r] = row_y[r - 1] + row_h[r - 1] + row_spacing;
    }

    for (std::size_t i = 0; i < cells.size(); ++i) {
        const std::size_t c = i % static_cast<std::size_t>(cols);
        const std::size_t r = i / static_cast<std::size_t>(cols);
        cells[i]->x = col_x[c];
        cells[i]->y = row_y[r];
        cells[i]->width = want_w(cells[i]);
        cells[i]->height = want_h(cells[i]);
    }

    implicit_width = col_x.back() + col_w.back();
    implicit_height = row_y.back() + row_h.back();
}

}  // namespace ps::ui

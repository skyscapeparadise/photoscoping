#pragma once

#include "ui/node.hpp"

namespace ps::ui {

// A row or a column.
//
// Sizing rule: a child with an explicit preferred size gets it; a
// child marked fill takes an equal share of what is left over; anything else
// keeps the size it already has.
class LinearLayout : public Node {
public:
    enum class Direction { kVertical, kHorizontal };

    explicit LinearLayout(Direction dir = Direction::kVertical) : direction_(dir) {}

    float spacing = 0.0f;
    // Cross-axis stretch, applied to every child unless it opts out via an
    // explicit preferred size.
    bool stretch_cross_axis = true;

protected:
    void layout_self() override;

private:
    Direction direction_;
};

// Row-major placement into `columns` columns. A column is as wide as its widest
// child and a row as tall as its tallest, which is what lets a grid of
// fixed-size cards and a grid of mixed content both lay out sensibly without
// either declaring track sizes.
class GridLayout : public Node {
public:
    int columns = 1;
    float row_spacing = 0.0f;
    float column_spacing = 0.0f;

protected:
    void layout_self() override;
};

}  // namespace ps::ui

#pragma once

#include <vector>

#include "gpu/draw_list.hpp"

namespace ps::editor {

// A line through `points`, `width` logical pixels wide, closed back to the
// first point if `closed`. Antialiased by a pixel of feather either side
// that fades to nothing, which is how the draw list's triangles are meant to
// be smoothed: they bypass its rounded-rect shader.
void stroke_polyline(gpu::DrawList& draw, const std::vector<float>& xy, bool closed, float width,
                     gpu::Color color);

}  // namespace ps::editor

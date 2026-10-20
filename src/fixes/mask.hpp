#pragma once

#include <cstdint>
#include <vector>

#include "fixes/fixes.hpp"

namespace ps::fixes {

// What the garbage mattes leave of the frame at `frame`: 1 inside any of
// them, 0 outside all, antialiased along their edges. `width` x `height` is
// the mask's own size, and image pixels are scaled by `scale` to reach it --
// half a pixel's precision is plenty for a mask that only ever multiplies a
// matte, and a quarter of the pixels to fill.
//
// Empty when there are no garbage mattes, which means nothing is removed.
std::vector<std::uint8_t> garbage_mask(const Fixes& fixes, std::int64_t frame, int width,
                                       int height, float scale);

}  // namespace ps::fixes

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

// Everything the fixes do to the matte at `frame`, as four bytes a pixel:
//
//   r  what the garbage mattes leave (all 1 without any)
//   g  m  } the paint strokes, folded together: alpha becomes m * alpha + k
//   b  k  }
//   a  unused
//
// Every stroke maps alpha to m * alpha + k -- painting out multiplies by
// (1 - c), painting in mixes toward 1 by c, where c is the brush's coverage
// -- and maps of that shape compose into one of the same shape, so any
// number of strokes in any order still come to two numbers a pixel. The
// garbage mattes apply after the paint: what they say is outside stays out.
//
// Sizes and scale as garbage_mask. Empty when there are no fixes live at
// `frame`, which means the matte as it is.
std::vector<std::uint8_t> fixes_mask(const Fixes& fixes, std::int64_t frame, int width, int height,
                                     float scale);

// A stroke's coverage at `frame`, 0..1, width x height at `scale`: the union
// of capsules along its points, the outer `softness` of the radius fading.
// Exposed for the tests.
std::vector<float> stroke_coverage(const Stroke& stroke, std::int64_t frame, int width, int height,
                                   float scale);

}  // namespace ps::fixes

#pragma once

#include <vector>

namespace ps::matting {

// Multilevel foreground estimation (Germer et al.), as pymatting's
// estimate_foreground_ml does it at its defaults: from a 1x1 image of the
// mean colours up to full size, each level a few Gauss-Seidel sweeps that fit
// every pixel's foreground and background to its alpha and its neighbours.
//
// Ported in pymatting's own order -- a pixel's update reads neighbours
// already updated in the same sweep -- and in its precision, float storage
// with some arithmetic in double where numba promotes it, so the result
// stays close to Python's rather than merely similar.
//
// `image` is interleaved RGB, width x height, 0..1; `alpha` is width x
// height. The result is the foreground, interleaved like the image.
std::vector<float> estimate_foreground_ml(const std::vector<float>& image,
                                          const std::vector<float>& alpha, int width,
                                          int height);

}  // namespace ps::matting

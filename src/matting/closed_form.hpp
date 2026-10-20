#pragma once

#include <string>
#include <vector>

namespace ps::matting {

// Closed-form matting (Levin, Lischinski and Weiss), as pymatting's
// estimate_alpha_cf does it at its defaults: the matting Laplacian over 3x3
// windows with epsilon 1e-7, its unknown part solved by conjugate gradients
// to a relative residual of 1e-7, preconditioned by a thresholded incomplete
// Cholesky factorisation. Ported line for line, so the answer and the number
// of iterations it takes to get there both stay where Python's are.
//
// `rgb` is the region's image, interleaved, width x height, 0..1. `trimap`
// is 1 for subject, 0 for background, anything between for unknown (pymatting
// splits at 0.1 and 0.9). The result is alpha for the whole region, the
// known pixels as the trimap has them, clamped to 0..1. Empty with a message
// in `error` if the solve fails to converge.
std::vector<double> estimate_alpha_cf(const std::vector<double>& rgb,
                                      const std::vector<double>& trimap, int width, int height,
                                      std::string* error);

}  // namespace ps::matting

#pragma once

#include <cstdint>
#include <vector>

// The image operations the matting stack is built from, each matching the
// Python stack's (tools/matting_stack.py) call it stands in for, so the two
// can be compared number for number. Single-channel images are row-major
// floats, width x height.
namespace ps::matting {

// PyTorch's bilinear resize, align_corners=False, as matting_bakeoff.upsample
// uses it -- source positions below zero clamped to zero, as PyTorch does for
// linear modes and not for cubic -- then clamped to 0..1 like that function.
std::vector<float> resize_bilinear(const std::vector<float>& src, int src_w, int src_h,
                                   int dst_w, int dst_h);

// scipy.ndimage.binary_dilation with its default cross, `iterations` times:
// every pixel within that many 4-connected steps of a set one. That is a
// Manhattan distance, so it is found exactly by a two-pass distance transform
// rather than by repeating the dilation.
std::vector<std::uint8_t> dilate_cross(const std::vector<std::uint8_t>& mask, int width,
                                       int height, int iterations);

// scipy.ndimage.gaussian_filter at its defaults: a kernel out to
// int(4 sigma + 0.5), normalised, edges mirrored about the half-pixel
// ("reflect": d c b a | a b c d).
std::vector<float> gaussian_blur(const std::vector<float>& src, int width, int height,
                                 float sigma);

// PyTorch's bilinear resize of planar channels, as F.interpolate does it --
// unclamped, for light that can run past 1.
std::vector<float> resize_bilinear_planes(const std::vector<float>& src, int src_w, int src_h,
                                          int dst_w, int dst_h, int channels);

// matting_stack.fill_plate: every pixel's colour from the nearest `known`
// ones, by push-pull -- averaged down a pyramid from half resolution, then
// each level's holes filled from the level below. Planar in and out.
std::vector<float> fill_plate(const std::vector<float>& planes, int width, int height,
                              const std::vector<std::uint8_t>& known);

}  // namespace ps::matting

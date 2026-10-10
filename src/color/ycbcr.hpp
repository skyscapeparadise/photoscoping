#pragma once

#include <array>

namespace ps::color {

// The YCbCr -> R'G'B' matrix a file was encoded with.
enum class YcbcrMatrix { kBt601, kBt709, kBt2020 };

// Legal ("limited", "video", "narrow") range puts black at code 16 and white
// at 235 (scaled for bit depth). Full range uses every code.
enum class SignalRange { kLimited, kFull };

const char* matrix_name(YcbcrMatrix matrix);
const char* range_name(SignalRange range);

// Y, Cb, Cr samples normalised to code / (2^depth - 1) -> full-range R'G'B'
// in 0..1, as one 3x4 affine (row-major, last column is the offset). Range
// expansion and the matrix are folded together so the shader does one
// multiply-add per channel.
//
// Full-range R'G'B' is what Canon's "Full in" LUTs expect: the standard
// decode of a legal-range file. Nothing else may touch the signal between
// this and the LUT.
using Affine3x4 = std::array<float, 12>;
Affine3x4 ycbcr_to_rgb(YcbcrMatrix matrix, SignalRange range, int bit_depth);

std::array<float, 3> apply_affine(const Affine3x4& m, const std::array<float, 3>& ycbcr);

}  // namespace ps::color

#include "color/ycbcr.hpp"

namespace ps::color {

const char* matrix_name(YcbcrMatrix matrix) {
    switch (matrix) {
        case YcbcrMatrix::kBt601:  return "BT.601";
        case YcbcrMatrix::kBt709:  return "BT.709";
        case YcbcrMatrix::kBt2020: return "BT.2020";
    }
    return "?";
}

const char* range_name(SignalRange range) {
    return range == SignalRange::kFull ? "full" : "limited";
}

Affine3x4 ycbcr_to_rgb(YcbcrMatrix matrix, SignalRange range, int bit_depth) {
    float kr = 0.2126f, kb = 0.0722f;
    if (matrix == YcbcrMatrix::kBt601) { kr = 0.299f; kb = 0.114f; }
    if (matrix == YcbcrMatrix::kBt2020) { kr = 0.2627f; kb = 0.0593f; }
    const float kg = 1.0f - kr - kb;

    // Y' and C' from the normalised sample s = code / max_code:
    //   Y' = s * y_gain + y_offset,  C' = s * c_gain + c_offset
    const float max_code = static_cast<float>((1 << bit_depth) - 1);
    const float k = static_cast<float>(1 << (bit_depth - 8));
    float y_gain, y_offset, c_gain, c_offset;
    if (range == SignalRange::kLimited) {
        y_gain = max_code / (219.0f * k);
        y_offset = -16.0f / 219.0f;
        c_gain = max_code / (224.0f * k);
        c_offset = -128.0f / 224.0f;
    } else {
        y_gain = 1.0f;
        y_offset = 0.0f;
        c_gain = 1.0f;
        // Full-range chroma is centred on 2^(depth-1), which is not exactly
        // the midpoint of 0..max_code.
        c_offset = -static_cast<float>(1 << (bit_depth - 1)) / max_code;
    }

    // R'G'B' from Y'CbCr (Cb, Cr in -0.5..0.5).
    const float r_cr = 2.0f * (1.0f - kr);
    const float b_cb = 2.0f * (1.0f - kb);
    const float g_cb = -2.0f * kb * (1.0f - kb) / kg;
    const float g_cr = -2.0f * kr * (1.0f - kr) / kg;

    auto row = [&](float cb, float cr) {
        return std::array<float, 4>{y_gain, cb * c_gain, cr * c_gain,
                                    y_offset + (cb + cr) * c_offset};
    };
    const auto r = row(0.0f, r_cr), g = row(g_cb, g_cr), b = row(b_cb, 0.0f);
    return {r[0], r[1], r[2], r[3], g[0], g[1], g[2], g[3], b[0], b[1], b[2], b[3]};
}

std::array<float, 3> apply_affine(const Affine3x4& m, const std::array<float, 3>& v) {
    return {m[0] * v[0] + m[1] * v[1] + m[2] * v[2] + m[3],
            m[4] * v[0] + m[5] * v[1] + m[6] * v[2] + m[7],
            m[8] * v[0] + m[9] * v[1] + m[10] * v[2] + m[11]};
}

}  // namespace ps::color

#include "matting/preprocess.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace ps::matting {

namespace {

constexpr float kMean[3] = {0.485f, 0.456f, 0.406f};
constexpr float kStd[3] = {0.229f, 0.224f, 0.225f};

// Rec.2020 -> Rec.709 primaries, linear: the same numbers as
// tools/matting_bakeoff.py's REC2020_TO_709, so the two agree to the bit.
constexpr float k2020To709[3][3] = {{1.6605f, -0.5876f, -0.0728f},
                                    {-0.1246f, 1.1329f, -0.0083f},
                                    {-0.0182f, -0.1006f, 1.1187f}};

float srgb_encode(float l) {
    l = std::clamp(l, 0.0f, 1.0f);
    return l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
}

// PyTorch's cubic convolution weights, a = -0.75.
std::array<float, 4> cubic_weights(float t) {
    constexpr float a = -0.75f;
    auto near = [&](float x) { return ((a + 2.0f) * x - (a + 3.0f)) * x * x + 1.0f; };
    auto far = [&](float x) { return ((a * x - 5.0f * a) * x + 8.0f * a) * x - 4.0f * a; };
    return {far(t + 1.0f), near(t), near(1.0f - t), far(2.0f - t)};
}

// One axis of the resize: for each output position, the four source indices
// (clamped at the edges, as PyTorch does) and their weights.
struct Taps {
    std::vector<std::array<int, 4>> index;
    std::vector<std::array<float, 4>> weight;
};

Taps taps_for(int in, int out) {
    Taps taps;
    taps.index.resize(out);
    taps.weight.resize(out);
    const float scale = static_cast<float>(in) / static_cast<float>(out);
    for (int o = 0; o < out; ++o) {
        // Pixel centres, align_corners=False.
        const float src = (static_cast<float>(o) + 0.5f) * scale - 0.5f;
        const int base = static_cast<int>(std::floor(src));
        taps.weight[o] = cubic_weights(src - static_cast<float>(base));
        for (int k = 0; k < 4; ++k) taps.index[o][k] = std::clamp(base - 1 + k, 0, in - 1);
    }
    return taps;
}

}  // namespace

std::vector<float> model_input(const std::vector<float>& rgba, int width, int height, int size) {
    // The frame as the model's colour: Rec.709, sRGB-encoded, planar.
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    std::vector<float> srgb(pixels * 3);
    for (std::size_t i = 0; i < pixels; ++i) {
        const float* p = &rgba[i * 4];
        for (int c = 0; c < 3; ++c) {
            const float l = k2020To709[c][0] * p[0] + k2020To709[c][1] * p[1] +
                            k2020To709[c][2] * p[2];
            srgb[c * pixels + i] = srgb_encode(l);
        }
    }

    // Separable bicubic: across, then down. PyTorch clamps the result to
    // 0..1 after both, which matting_bakeoff.py does too.
    const Taps across = taps_for(width, size);
    const Taps down = taps_for(height, size);
    std::vector<float> wide(static_cast<std::size_t>(height) * size);
    std::vector<float> out(static_cast<std::size_t>(3) * size * size);
    for (int c = 0; c < 3; ++c) {
        const float* plane = &srgb[c * pixels];
        for (int y = 0; y < height; ++y) {
            const float* row = plane + static_cast<std::size_t>(y) * width;
            for (int x = 0; x < size; ++x) {
                const auto& i = across.index[x];
                const auto& w = across.weight[x];
                wide[static_cast<std::size_t>(y) * size + x] =
                    row[i[0]] * w[0] + row[i[1]] * w[1] + row[i[2]] * w[2] + row[i[3]] * w[3];
            }
        }
        float* dst = &out[static_cast<std::size_t>(c) * size * size];
        for (int y = 0; y < size; ++y) {
            const auto& i = down.index[y];
            const auto& w = down.weight[y];
            const float* r0 = &wide[static_cast<std::size_t>(i[0]) * size];
            const float* r1 = &wide[static_cast<std::size_t>(i[1]) * size];
            const float* r2 = &wide[static_cast<std::size_t>(i[2]) * size];
            const float* r3 = &wide[static_cast<std::size_t>(i[3]) * size];
            for (int x = 0; x < size; ++x) {
                const float v = r0[x] * w[0] + r1[x] * w[1] + r2[x] * w[2] + r3[x] * w[3];
                dst[static_cast<std::size_t>(y) * size + x] =
                    (std::clamp(v, 0.0f, 1.0f) - kMean[c]) / kStd[c];
            }
        }
    }
    return out;
}

}  // namespace ps::matting

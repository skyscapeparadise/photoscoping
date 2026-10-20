#include "matting/foreground.hpp"

#include <algorithm>
#include <cmath>

namespace ps::matting {

namespace {

// pymatting's _resize_nearest, any channel count.
void resize_nearest(std::vector<float>& dst, int dw, int dh, const std::vector<float>& src, int sw,
                    int sh, int depth) {
    dst.resize(static_cast<std::size_t>(dw) * dh * depth);
    for (int y = 0; y < dh; ++y) {
        const int ys = std::clamp(static_cast<int>(static_cast<long long>(y) * sh / dh), 0, sh - 1);
        for (int x = 0; x < dw; ++x) {
            const int xs =
                std::clamp(static_cast<int>(static_cast<long long>(x) * sw / dw), 0, sw - 1);
            for (int c = 0; c < depth; ++c)
                dst[(static_cast<std::size_t>(y) * dw + x) * depth + c] =
                    src[(static_cast<std::size_t>(ys) * sw + xs) * depth + c];
        }
    }
}

}  // namespace

std::vector<float> estimate_foreground_ml(const std::vector<float>& image,
                                          const std::vector<float>& alpha, int w0, int h0) {
    constexpr int depth = 3;
    constexpr float regularization = 1e-5f;
    constexpr int small_iterations = 10, big_iterations = 2, small_size = 32;
    constexpr float gradient_weight = 1.0f;

    // The mean colour of the solid subject and of the clear background.
    float f_mean[depth] = {}, b_mean[depth] = {};
    long long f_count = 0, b_count = 0;
    const std::size_t pixels = static_cast<std::size_t>(w0) * h0;
    for (std::size_t i = 0; i < pixels; ++i) {
        if (alpha[i] > 0.9f) {
            for (int c = 0; c < depth; ++c) f_mean[c] += image[i * depth + c];
            ++f_count;
        }
        if (alpha[i] < 0.1f) {
            for (int c = 0; c < depth; ++c) b_mean[c] += image[i * depth + c];
            ++b_count;
        }
    }
    for (int c = 0; c < depth; ++c) {
        f_mean[c] /= static_cast<float>(f_count + 1e-5);
        b_mean[c] /= static_cast<float>(b_count + 1e-5);
    }

    std::vector<float> f_prev(f_mean, f_mean + depth), b_prev(b_mean, b_mean + depth);
    int w_prev = 1, h_prev = 1;
    const int levels = static_cast<int>(std::ceil(std::log2(static_cast<double>(std::max(w0, h0)))));
    std::vector<float> img, a, f, b;
    const int dx[4] = {-1, 1, 0, 0};
    const int dy[4] = {0, 0, -1, 1};
    for (int level = 0; level <= levels; ++level) {
        // Python's round(), halves to even.
        const int w = static_cast<int>(
            std::nearbyint(std::pow(static_cast<double>(w0), static_cast<double>(level) / levels)));
        const int h = static_cast<int>(
            std::nearbyint(std::pow(static_cast<double>(h0), static_cast<double>(level) / levels)));
        resize_nearest(img, w, h, image, w0, h0, depth);
        resize_nearest(a, w, h, alpha, w0, h0, 1);
        resize_nearest(f, w, h, f_prev, w_prev, h_prev, depth);
        resize_nearest(b, w, h, b_prev, w_prev, h_prev, depth);
        const int iterations = (w <= small_size && h <= small_size) ? small_iterations : big_iterations;

        float rhs[2][depth];
        for (int iteration = 0; iteration < iterations; ++iteration) {
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const std::size_t p = static_cast<std::size_t>(y) * w + x;
                    // numba's types: a0 is the array's float; 1.0 - a0 is a
                    // double, and so is everything mixed with it.
                    const float a0 = a[p];
                    const double a1 = 1.0 - a0;
                    float a00 = a0 * a0;
                    const double a01 = a0 * a1;
                    double a11 = a1 * a1;
                    for (int c = 0; c < depth; ++c) {
                        rhs[0][c] = a0 * img[p * depth + c];
                        rhs[1][c] = static_cast<float>(a1 * img[p * depth + c]);
                    }
                    for (int d = 0; d < 4; ++d) {
                        const int x2 = std::clamp(x + dx[d], 0, w - 1);
                        const int y2 = std::clamp(y + dy[d], 0, h - 1);
                        const std::size_t q = static_cast<std::size_t>(y2) * w + x2;
                        const float gradient = std::abs(a0 - a[q]);
                        const float da = regularization + gradient_weight * gradient;
                        a00 += da;
                        a11 += da;
                        for (int c = 0; c < depth; ++c) {
                            rhs[0][c] += da * f[q * depth + c];
                            rhs[1][c] += da * b[q * depth + c];
                        }
                    }
                    const double determinant = a00 * a11 - a01 * a01;
                    const double inv_det = 1.0 / determinant;
                    const double b00 = inv_det * a11;
                    const double b01 = inv_det * -a01;
                    const double b11 = inv_det * a00;
                    for (int c = 0; c < depth; ++c) {
                        const double fc = b00 * rhs[0][c] + b01 * rhs[1][c];
                        const double bc = b01 * rhs[0][c] + b11 * rhs[1][c];
                        f[p * depth + c] = static_cast<float>(std::clamp(fc, 0.0, 1.0));
                        b[p * depth + c] = static_cast<float>(std::clamp(bc, 0.0, 1.0));
                    }
                }
            }
        }
        f_prev = f;
        b_prev = b;
        w_prev = w;
        h_prev = h;
    }
    return f;
}

}  // namespace ps::matting

#include "matting/image_ops.hpp"
#include "matting/parallel.hpp"

#include <algorithm>
#include <cmath>

namespace ps::matting {

std::vector<float> resize_bilinear(const std::vector<float>& src, int src_w, int src_h,
                                   int dst_w, int dst_h) {
    struct Tap {
        int i0, i1;
        float w1;
    };
    auto taps = [](int in, int out) {
        std::vector<Tap> t(out);
        const float scale = static_cast<float>(in) / static_cast<float>(out);
        for (int o = 0; o < out; ++o) {
            const float s = std::max(0.0f, (static_cast<float>(o) + 0.5f) * scale - 0.5f);
            const int i0 = std::min(static_cast<int>(s), in - 1);
            t[o] = {i0, std::min(i0 + 1, in - 1), s - static_cast<float>(i0)};
        }
        return t;
    };
    const std::vector<Tap> xs = taps(src_w, dst_w);
    const std::vector<Tap> ys = taps(src_h, dst_h);
    std::vector<float> out(static_cast<std::size_t>(dst_w) * dst_h);
    for (int y = 0; y < dst_h; ++y) {
        const Tap& ty = ys[y];
        const float* r0 = &src[static_cast<std::size_t>(ty.i0) * src_w];
        const float* r1 = &src[static_cast<std::size_t>(ty.i1) * src_w];
        float* row = &out[static_cast<std::size_t>(y) * dst_w];
        for (int x = 0; x < dst_w; ++x) {
            const Tap& tx = xs[x];
            const float top = r0[tx.i0] + (r0[tx.i1] - r0[tx.i0]) * tx.w1;
            const float bottom = r1[tx.i0] + (r1[tx.i1] - r1[tx.i0]) * tx.w1;
            row[x] = std::clamp(top + (bottom - top) * ty.w1, 0.0f, 1.0f);
        }
    }
    return out;
}

std::vector<std::uint8_t> dilate_cross(const std::vector<std::uint8_t>& mask, int width,
                                       int height, int iterations) {
    // Manhattan distance to the nearest set pixel, forward then back.
    const int far = width + height;
    std::vector<int> d(static_cast<std::size_t>(width) * height);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * width + x;
            int v = mask[i] ? 0 : far;
            if (x > 0) v = std::min(v, d[i - 1] + 1);
            if (y > 0) v = std::min(v, d[i - width] + 1);
            d[i] = v;
        }
    for (int y = height - 1; y >= 0; --y)
        for (int x = width - 1; x >= 0; --x) {
            const std::size_t i = static_cast<std::size_t>(y) * width + x;
            int v = d[i];
            if (x + 1 < width) v = std::min(v, d[i + 1] + 1);
            if (y + 1 < height) v = std::min(v, d[i + width] + 1);
            d[i] = v;
        }
    std::vector<std::uint8_t> out(d.size());
    for (std::size_t i = 0; i < d.size(); ++i) out[i] = d[i] <= iterations ? 1 : 0;
    return out;
}

namespace {

// Index i folded back into [0, n) by mirroring at the edges -- d c b a | a b c
// d | d c b a -- as many times as it takes.
inline int mirror(int i, int n) {
    const int period = 2 * n;
    i %= period;
    if (i < 0) i += period;
    return i < n ? i : period - 1 - i;
}

// The widths of three box blurs that together make a Gaussian of sigma
// (Kovesi's "boxes for Gauss"): odd widths, the first m of them one size, the
// rest the next odd size up, so the variance comes out right.
void gauss_boxes(float sigma, int radii[3]) {
    constexpr int n = 3;
    const double s2 = static_cast<double>(sigma) * sigma;
    int wl = static_cast<int>(std::floor(std::sqrt(12.0 * s2 / n + 1.0)));
    if (wl % 2 == 0) --wl;
    const int wu = wl + 2;
    const int m = static_cast<int>(
        std::lround((12.0 * s2 - n * wl * wl - 4.0 * n * wl - 3.0 * n) / (-4.0 * wl - 4.0)));
    const int small = std::clamp(m, 0, n);
    for (int i = 0; i < n; ++i) radii[i] = ((i < small ? wl : wu) - 1) / 2;
}

// One box blur of radius r along the rows, src to dst, mirrored at the ends.
void box_rows(const float* src, float* dst, int width, int height, int r) {
    const double inv = 1.0 / (2 * r + 1);
    parallel_for(static_cast<std::size_t>(height), [&](std::size_t y0, std::size_t y1) {
        for (std::size_t y = y0; y < y1; ++y) {
            const float* row = src + y * width;
            float* out = dst + y * width;
            double sum = 0.0;
            for (int k = -r; k <= r; ++k) sum += row[mirror(k, width)];
            for (int x = 0; x < width; ++x) {
                out[x] = static_cast<float>(sum * inv);
                sum += row[mirror(x + r + 1, width)] - row[mirror(x - r, width)];
            }
        }
    }, 8);
}

// The same down the columns, a strip of columns at a time so rows are read
// in order.
void box_columns(const float* src, float* dst, int width, int height, int r) {
    constexpr int kStrip = 64;
    const double inv = 1.0 / (2 * r + 1);
    const std::size_t strips = (static_cast<std::size_t>(width) + kStrip - 1) / kStrip;
    parallel_for(strips, [&](std::size_t s0, std::size_t s1) {
        std::vector<double> sum(kStrip);
        for (std::size_t s = s0; s < s1; ++s) {
            const int x0 = static_cast<int>(s) * kStrip, x1 = std::min(width, x0 + kStrip);
            const int n = x1 - x0;
            std::fill(sum.begin(), sum.begin() + n, 0.0);
            for (int k = -r; k <= r; ++k) {
                const float* row = src + static_cast<std::size_t>(mirror(k, height)) * width + x0;
                for (int x = 0; x < n; ++x) sum[x] += row[x];
            }
            for (int y = 0; y < height; ++y) {
                float* out = dst + static_cast<std::size_t>(y) * width + x0;
                for (int x = 0; x < n; ++x) out[x] = static_cast<float>(sum[x] * inv);
                const float* in = src + static_cast<std::size_t>(mirror(y + r + 1, height)) * width + x0;
                const float* gone = src + static_cast<std::size_t>(mirror(y - r, height)) * width + x0;
                for (int x = 0; x < n; ++x) sum[x] += in[x] - gone[x];
            }
        }
    }, 1);
}

}  // namespace

std::vector<float> gaussian_blur(const std::vector<float>& src, int width, int height,
                                 float sigma) {
    if (width <= 0 || height <= 0 || sigma <= 0.0f) return src;

    // Wide: three box blurs each way, whose cost does not grow with sigma --
    // a direct kernel 4 sigma either side is hundreds of taps by then. Within
    // a percent or two of the true Gaussian, and smooth.
    if (sigma >= 3.0f) {
        int radii[3];
        gauss_boxes(sigma, radii);
        std::vector<float> a(src), b(src.size());
        for (int r : radii) {
            box_columns(a.data(), b.data(), width, height, r);
            box_rows(b.data(), a.data(), width, height, r);
        }
        return a;
    }

    // Narrow: the exact kernel, 4 sigma either side, as scipy makes it.
    const int radius = static_cast<int>(4.0 * sigma + 0.5);
    std::vector<double> kernel(2 * radius + 1);
    double sum = 0.0;
    for (int k = -radius; k <= radius; ++k) {
        kernel[k + radius] = std::exp(-0.5 * k * k / (static_cast<double>(sigma) * sigma));
        sum += kernel[k + radius];
    }
    for (double& k : kernel) k /= sum;

    // Down the columns, then along the rows: scipy's axis order. Summed in
    // double, as scipy does. A whole row accumulated at a time, so the source
    // is read in order rather than down a column.
    std::vector<float> mid(src.size());
    parallel_for(static_cast<std::size_t>(height), [&](std::size_t y0, std::size_t y1) {
        std::vector<double> acc(width);
        for (std::size_t y = y0; y < y1; ++y) {
            std::fill(acc.begin(), acc.end(), 0.0);
            for (int k = -radius; k <= radius; ++k) {
                const double w = kernel[k + radius];
                const float* row = &src[static_cast<std::size_t>(mirror(static_cast<int>(y) + k, height)) * width];
                for (int x = 0; x < width; ++x) acc[x] += w * row[x];
            }
            float* dst = &mid[y * width];
            for (int x = 0; x < width; ++x) dst[x] = static_cast<float>(acc[x]);
        }
    }, 8);
    std::vector<float> out(src.size());
    parallel_for(static_cast<std::size_t>(height), [&](std::size_t y0, std::size_t y1) {
        for (std::size_t y = y0; y < y1; ++y) {
            const float* row = &mid[y * width];
            float* dst = &out[y * width];
            // The middle without folding; the ends folded.
            const int lo = std::min(radius, width), hi = std::max(lo, width - radius);
            auto at = [&](int x) {
                double acc = 0.0;
                for (int k = -radius; k <= radius; ++k) acc += kernel[k + radius] * row[mirror(x + k, width)];
                return static_cast<float>(acc);
            };
            for (int x = 0; x < lo; ++x) dst[x] = at(x);
            for (int x = lo; x < hi; ++x) {
                double acc = 0.0;
                const float* p = row + x - radius;
                for (int k = 0; k <= 2 * radius; ++k) acc += kernel[k] * p[k];
                dst[x] = static_cast<float>(acc);
            }
            for (int x = hi; x < width; ++x) dst[x] = at(x);
        }
    }, 8);
    return out;
}

std::vector<float> resize_bilinear_planes(const std::vector<float>& src, int src_w, int src_h,
                                          int dst_w, int dst_h, int channels) {
    struct Tap {
        int i0, i1;
        float w1;
    };
    auto taps = [](int in, int out) {
        std::vector<Tap> t(out);
        const float scale = static_cast<float>(in) / static_cast<float>(out);
        for (int o = 0; o < out; ++o) {
            const float s = std::max(0.0f, (static_cast<float>(o) + 0.5f) * scale - 0.5f);
            const int i0 = std::min(static_cast<int>(s), in - 1);
            t[o] = {i0, std::min(i0 + 1, in - 1), s - static_cast<float>(i0)};
        }
        return t;
    };
    const std::vector<Tap> xs = taps(src_w, dst_w);
    const std::vector<Tap> ys = taps(src_h, dst_h);
    const std::size_t src_plane = static_cast<std::size_t>(src_w) * src_h;
    const std::size_t dst_plane = static_cast<std::size_t>(dst_w) * dst_h;
    std::vector<float> out(dst_plane * channels);
    for (int c = 0; c < channels; ++c) {
        const float* plane = &src[c * src_plane];
        for (int y = 0; y < dst_h; ++y) {
            const Tap& ty = ys[y];
            const float* r0 = plane + static_cast<std::size_t>(ty.i0) * src_w;
            const float* r1 = plane + static_cast<std::size_t>(ty.i1) * src_w;
            float* row = &out[c * dst_plane + static_cast<std::size_t>(y) * dst_w];
            for (int x = 0; x < dst_w; ++x) {
                const Tap& tx = xs[x];
                const float top = r0[tx.i0] + (r0[tx.i1] - r0[tx.i0]) * tx.w1;
                const float bottom = r1[tx.i0] + (r1[tx.i1] - r1[tx.i0]) * tx.w1;
                row[x] = top + (bottom - top) * ty.w1;
            }
        }
    }
    return out;
}

namespace {

// F.avg_pool2d with a 2x2 kernel and stride 2, planar. With `ceil` a window
// that runs off the far edge still makes an output, averaging only what is
// inside -- PyTorch counts no padding it was not given.
std::vector<float> avg_pool2(const std::vector<float>& src, int w, int h, int channels, bool ceil,
                             int* out_w, int* out_h) {
    const int ow = ceil ? (w + 1) / 2 : w / 2;
    const int oh = ceil ? (h + 1) / 2 : h / 2;
    std::vector<float> out(static_cast<std::size_t>(ow) * oh * channels);
    for (int c = 0; c < channels; ++c) {
        const float* plane = &src[static_cast<std::size_t>(c) * w * h];
        for (int y = 0; y < oh; ++y) {
            const int y1 = std::min(2 * y + 2, h);
            for (int x = 0; x < ow; ++x) {
                const int x1 = std::min(2 * x + 2, w);
                float sum = 0.0f;
                for (int sy = 2 * y; sy < y1; ++sy)
                    for (int sx = 2 * x; sx < x1; ++sx)
                        sum += plane[static_cast<std::size_t>(sy) * w + sx];
                out[(static_cast<std::size_t>(c) * oh + y) * ow + x] =
                    sum / static_cast<float>((y1 - 2 * y) * (x1 - 2 * x));
            }
        }
    }
    *out_w = ow;
    *out_h = oh;
    return out;
}

}  // namespace

std::vector<float> fill_plate(const std::vector<float>& planes, int width, int height,
                              const std::vector<std::uint8_t>& known) {
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    std::vector<float> masked(planes.size()), mask(pixels);
    for (std::size_t i = 0; i < pixels; ++i) mask[i] = known[i] ? 1.0f : 0.0f;
    for (int c = 0; c < 3; ++c)
        for (std::size_t i = 0; i < pixels; ++i) masked[c * pixels + i] = planes[c * pixels + i] * mask[i];

    struct Level {
        std::vector<float> sum, weight;
        int w, h;
    };
    std::vector<Level> levels;
    Level first;
    first.sum = avg_pool2(masked, width, height, 3, false, &first.w, &first.h);
    int ww = 0, wh = 0;
    first.weight = avg_pool2(mask, width, height, 1, false, &ww, &wh);
    levels.push_back(std::move(first));
    while (std::min(levels.back().w, levels.back().h) > 4) {
        const Level& prev = levels.back();
        Level next;
        next.sum = avg_pool2(prev.sum, prev.w, prev.h, 3, true, &next.w, &next.h);
        next.weight = avg_pool2(prev.weight, prev.w, prev.h, 1, true, &ww, &wh);
        levels.push_back(std::move(next));
    }

    auto mean_of = [](const Level& l, std::size_t i, int c) {
        const std::size_t n = static_cast<std::size_t>(l.w) * l.h;
        return l.sum[c * n + i] / std::max(l.weight[i], 1e-6f);
    };
    const Level& top = levels.back();
    std::vector<float> colour(top.sum.size());
    const std::size_t top_n = static_cast<std::size_t>(top.w) * top.h;
    for (int c = 0; c < 3; ++c)
        for (std::size_t i = 0; i < top_n; ++i) colour[c * top_n + i] = mean_of(top, i, c);
    int cw = top.w, ch = top.h;
    for (int li = static_cast<int>(levels.size()) - 2; li >= 0; --li) {
        const Level& l = levels[li];
        const std::vector<float> up = resize_bilinear_planes(colour, cw, ch, l.w, l.h, 3);
        const std::size_t n = static_cast<std::size_t>(l.w) * l.h;
        colour.assign(n * 3, 0.0f);
        for (std::size_t i = 0; i < n; ++i) {
            // Mostly-known cells keep their own value; hidden ones take the
            // coarser level's.
            const float trust = std::clamp(l.weight[i] * 4.0f, 0.0f, 1.0f);
            for (int c = 0; c < 3; ++c)
                colour[c * n + i] = mean_of(l, i, c) * trust + up[c * n + i] * (1.0f - trust);
        }
        cw = l.w;
        ch = l.h;
    }
    return resize_bilinear_planes(colour, cw, ch, width, height, 3);
}

}  // namespace ps::matting

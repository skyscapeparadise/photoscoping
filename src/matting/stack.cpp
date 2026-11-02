#include "matting/stack.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>

#include "matting/closed_form.hpp"
#include "matting/foreground.hpp"
#include "matting/image_ops.hpp"

namespace ps::matting {

namespace {

constexpr int kWholeSize = 2048;
constexpr int kTileW = 2048;
constexpr int kTileH = 1152;

std::string model_path(const std::string& dir, int w, int h) {
    return (std::filesystem::path(dir) /
            ("birefnet_hr_matting_" + std::to_string(w) + "x" + std::to_string(h) + "_fp16.onnx"))
        .string();
}

}  // namespace

float MattingStack::resolution_scale(int width, int height) {
    return std::max(1.0f, static_cast<float>(std::max(width, height)) / 3840.0f);
}

std::vector<int> tile_starts(int length, int tile, int min_overlap) {
    if (length <= tile) return {0};
    const int n = 1 + static_cast<int>(std::ceil(static_cast<double>(length - tile) /
                                                 (tile - min_overlap)));
    std::vector<int> starts(n);
    // Python's round(), which takes halves to even.
    for (int i = 0; i < n; ++i)
        starts[i] = static_cast<int>(std::nearbyint(static_cast<double>(i) * (length - tile) / (n - 1)));
    return starts;
}

bool MattingStack::load(const std::string& model_dir, MatteModel::Backend backend,
                        const std::string& cache_dir, std::string* error) {
    return whole_.load(model_path(model_dir, kWholeSize, kWholeSize), backend, cache_dir, error) &&
           tile_.load(model_path(model_dir, kTileW, kTileH), backend, cache_dir, error);
}

std::vector<float> MattingStack::coarse(const MattingFrame& frame, std::string* error) {
    const std::vector<float> alpha =
        whole_.run(model_input(frame, whole_.width(), whole_.height()), error);
    if (alpha.empty()) return {};
    return resize_bilinear(alpha, whole_.width(), whole_.height(), frame.width, frame.height);
}

std::vector<float> MattingStack::tiled(const MattingFrame& frame, const std::vector<float>& coarse,
                                       std::string* error) {
    const int w = frame.width, h = frame.height;
    const int tw = tile_.width(), th = tile_.height();
    const std::size_t pixels = static_cast<std::size_t>(w) * h;

    // A tile sees part of the subject without the whole, so it is trusted for
    // edge detail and not for what the subject is: only in the coarse
    // matte's soft band, widened, with the band itself feathered so the
    // change from one to the other does not show. Worked out first, so a
    // tile with none of the band in it -- open wall, solid subject -- is
    // never run: whatever it said would be thrown away.
    std::vector<std::uint8_t> soft(pixels);
    for (std::size_t i = 0; i < pixels; ++i) soft[i] = coarse[i] > 0.02f && coarse[i] < 0.98f;
    const float scale = resolution_scale(w, h);
    const std::vector<std::uint8_t> band =
        dilate_cross(soft, w, h, static_cast<int>(std::lround(24.0f * scale)));
    std::vector<float> band_f(pixels);
    for (std::size_t i = 0; i < pixels; ++i) band_f[i] = band[i];
    const std::vector<float> blend = gaussian_blur(band_f, w, h, 8.0f * scale);

    // Each tile weighted by its distance in from its own edge, squared, so
    // where tiles overlap each one fades out as the next takes over.
    std::vector<float> feather(static_cast<std::size_t>(tw) * th);
    for (int y = 0; y < th; ++y)
        for (int x = 0; x < tw; ++x) {
            const float ramp = static_cast<float>(std::min({y + 1, th - y, x + 1, tw - x}));
            feather[static_cast<std::size_t>(y) * tw + x] = ramp * ramp;
        }

    std::vector<float> acc(pixels, 0.0f), weight(pixels, 0.0f);
    for (int y0 : tile_starts(h, th)) {
        for (int x0 : tile_starts(w, tw)) {
            // The part of the tile inside the frame; what padding added is
            // dropped.
            const int ch = std::min(th, h - y0), cw = std::min(tw, w - x0);
            bool needed = false;
            for (int y = 0; y < ch && !needed; ++y) {
                const float* row = &blend[static_cast<std::size_t>(y0 + y) * w + x0];
                for (int x = 0; x < cw; ++x)
                    if (row[x] > 0.0f) {
                        needed = true;
                        break;
                    }
            }
            if (!needed) continue;
            const std::vector<float> alpha = tile_.run(tile_input(frame, x0, y0, tw, th), error);
            if (alpha.empty()) return {};
            for (int y = 0; y < ch; ++y)
                for (int x = 0; x < cw; ++x) {
                    const std::size_t t = static_cast<std::size_t>(y) * tw + x;
                    const std::size_t f = static_cast<std::size_t>(y0 + y) * w + (x0 + x);
                    acc[f] += std::clamp(alpha[t], 0.0f, 1.0f) * feather[t];
                    weight[f] += feather[t];
                }
        }
    }

    std::vector<float> out(pixels);
    for (std::size_t i = 0; i < pixels; ++i) {
        const float b = std::clamp(blend[i], 0.0f, 1.0f);
        if (b <= 0.0f) {
            out[i] = coarse[i];
            continue;
        }
        const float tiles = acc[i] / std::max(weight[i], 1e-6f);
        out[i] = b * tiles + (1.0f - b) * coarse[i];
    }
    return out;
}

std::vector<float> MattingStack::closer(const MattingFrame& frame, const std::vector<float>& base,
                                        int x, int y, int w, int h, bool wide, std::string* error,
                                        const std::vector<float>* wall, float tolerance) {
    const MattingFrame crop = crop_frame(frame, x, y, w, h);
    MatteModel& model = wide ? tile_ : whole_;
    std::vector<float> look = model.run(model_input(crop, model.width(), model.height()), error);
    if (look.empty()) return {};
    look = resize_bilinear(look, model.width(), model.height(), w, h);

    const std::size_t pixels = static_cast<std::size_t>(w) * h;
    std::vector<float> under(pixels);
    for (int row = 0; row < h; ++row)
        std::copy_n(&base[static_cast<std::size_t>(y + row) * frame.width + x], w,
                    &under[static_cast<std::size_t>(row) * w]);

    // The same widening and feather the tiled stage uses, in crop pixels --
    // which are the frame's, so scaled for the frame's size, not the crop's.
    const float scale = resolution_scale(frame.width, frame.height);
    std::vector<std::uint8_t> soft(pixels);
    for (std::size_t i = 0; i < pixels; ++i) {
        const float a = std::clamp(look[i], 0.0f, 1.0f);
        soft[i] = (a > 0.02f && a < 0.98f) || (under[i] > 0.02f && under[i] < 0.98f);
    }
    const std::vector<std::uint8_t> band =
        dilate_cross(soft, w, h, static_cast<int>(std::lround(24.0f * scale)));
    std::vector<float> band_f(pixels);
    for (std::size_t i = 0; i < pixels; ++i) band_f[i] = band[i];
    const std::vector<float> blend = gaussian_blur(band_f, w, h, 8.0f * scale);
    std::vector<float> fused(pixels);
    for (std::size_t i = 0; i < pixels; ++i) {
        const float b = std::clamp(blend[i], 0.0f, 1.0f);
        fused[i] = b * std::clamp(look[i], 0.0f, 1.0f) + (1.0f - b) * under[i];
    }
    if (!wall) return MattingStack::band(crop, fused, error, nullptr, 0.0f, scale);
    // The plate's part under the crop.
    const std::size_t source = static_cast<std::size_t>(frame.width) * frame.height;
    std::vector<float> wall_crop(pixels * 3);
    for (int c = 0; c < 3; ++c)
        for (int row = 0; row < h; ++row)
            std::copy_n(&(*wall)[c * source + static_cast<std::size_t>(y + row) * frame.width + x], w,
                        &wall_crop[c * pixels + static_cast<std::size_t>(row) * w]);
    return MattingStack::band(crop, fused, error, &wall_crop, tolerance, scale);
}

std::vector<float> MattingStack::band(const MattingFrame& frame, const std::vector<float>& alpha,
                                      std::string* error, const std::vector<float>* plate_wall,
                                      float tolerance, float scale) {
    const int w = frame.width, h = frame.height;
    const std::size_t pixels = static_cast<std::size_t>(w) * h;

    // The trimap: subject and wall split at half, and unknown wherever the
    // matte is soft, widened.
    std::vector<std::uint8_t> soft(pixels);
    for (std::size_t i = 0; i < pixels; ++i) soft[i] = alpha[i] > 0.03f && alpha[i] < 0.97f;
    if (scale <= 0.0f) scale = resolution_scale(w, h);
    const std::vector<std::uint8_t> unknown =
        dilate_cross(soft, w, h, static_cast<int>(std::lround(10.0f * scale)));
    std::vector<double> trimap(pixels);
    for (std::size_t i = 0; i < pixels; ++i)
        trimap[i] = unknown[i] ? 0.5 : (alpha[i] >= 0.5f ? 1.0 : 0.0);
    // With a plate: what the matte calls mostly wall and looks exactly like
    // the wall is wall, which pins the solve down where it is otherwise
    // freest -- the haze just outside an edge.
    if (plate_wall) {
        for (std::size_t i = 0; i < pixels; ++i) {
            if (!unknown[i] || alpha[i] >= 0.5f) continue;
            float d2 = 0.0f;
            for (int c = 0; c < 3; ++c) {
                const float d = frame.linear[c * pixels + i] - (*plate_wall)[c * pixels + i];
                d2 += d * d;
            }
            if (d2 < tolerance * tolerance) trimap[i] = 0.0;
        }
    }

    // Solved in padded cells, so the full-resolution problem stays small.
    // A cell with nothing unknown, or without both subject and wall to solve
    // between, keeps the matte as it is.
    //
    // Each cell's answer is kept over its padding too, fading out linearly
    // across it while the next cell's fades in: two neighbours' weights add
    // to one, so a cell's edge is never a hard cut. Solved independently,
    // cells disagree across a wide soft region -- a motion-blurred hand at 8K
    // spans several -- and cut at their edges, that showed as straight steps.
    // Where a neighbour was not solved, the matte as it was makes up the rest.
    constexpr int kCell = 512;
    const int pad = static_cast<int>(std::lround(48.0f * scale));
    const float ramp = static_cast<float>(2 * pad);
    std::vector<float> acc(pixels, 0.0f), coverage(pixels, 0.0f);
    for (int y0 = 0; y0 < h; y0 += kCell) {
        for (int x0 = 0; x0 < w; x0 += kCell) {
            const int ch = std::min(kCell, h - y0), cw = std::min(kCell, w - x0);
            bool any_unknown = false;
            for (int y = y0; y < y0 + ch && !any_unknown; ++y)
                for (int x = x0; x < x0 + cw; ++x)
                    if (unknown[static_cast<std::size_t>(y) * w + x]) {
                        any_unknown = true;
                        break;
                    }
            if (!any_unknown) continue;
            const int ya = std::max(y0 - pad, 0), yb = std::min(y0 + kCell + pad, h);
            const int xa = std::max(x0 - pad, 0), xb = std::min(x0 + kCell + pad, w);
            const int rw = xb - xa, rh = yb - ya;
            std::vector<double> region_trimap(static_cast<std::size_t>(rw) * rh);
            std::vector<double> region_rgb(region_trimap.size() * 3);
            bool has_fg = false, has_bg = false;
            for (int y = 0; y < rh; ++y)
                for (int x = 0; x < rw; ++x) {
                    const std::size_t s = static_cast<std::size_t>(ya + y) * w + (xa + x);
                    const std::size_t d = static_cast<std::size_t>(y) * rw + x;
                    region_trimap[d] = trimap[s];
                    has_fg = has_fg || trimap[s] == 1.0;
                    has_bg = has_bg || trimap[s] == 0.0;
                    for (int c = 0; c < 3; ++c) region_rgb[d * 3 + c] = frame.plane(c)[s];
                }
            if (!has_fg || !has_bg) continue;
            const std::vector<double> a = estimate_alpha_cf(region_rgb, region_trimap, rw, rh, error);
            if (a.empty()) return {};
            // The weight across, and down: a ramp at each side that meets
            // another cell, none at the frame's edge.
            auto weight_at = [&](int p, int lo, int hi, int limit) {
                float t = 1.0f;
                if (lo > 0) t = std::min(t, (static_cast<float>(p - lo) + 0.5f) / ramp);
                if (hi < limit) t = std::min(t, (static_cast<float>(hi - p) - 0.5f) / ramp);
                return std::clamp(t, 0.0f, 1.0f);
            };
            for (int y = 0; y < rh; ++y) {
                const float wy = weight_at(ya + y, ya, yb, h);
                for (int x = 0; x < rw; ++x) {
                    const float wt = wy * weight_at(xa + x, xa, xb, w);
                    if (wt <= 0.0f) continue;
                    const std::size_t s = static_cast<std::size_t>(ya + y) * w + (xa + x);
                    acc[s] += wt * static_cast<float>(a[static_cast<std::size_t>(y) * rw + x]);
                    coverage[s] += wt;
                }
            }
        }
    }
    std::vector<float> solved(pixels);
    for (std::size_t i = 0; i < pixels; ++i) {
        const float c = coverage[i];
        solved[i] = c >= 1.0f ? acc[i] / c : acc[i] + (1.0f - c) * alpha[i];
    }

    // How far to trust the solve: by how different the subject's colour is
    // from the wall's there. Wall is background that looks like wall -- stray
    // hair outside the matte is background to it, but nearly black.
    std::vector<float> luma(pixels);
    for (std::size_t i = 0; i < pixels; ++i)
        luma[i] = frame.linear[i] * 0.2126f + frame.linear[pixels + i] * 0.7152f +
                  frame.linear[2 * pixels + i] * 0.0722f;
    std::vector<float> background_luma;
    for (std::size_t i = 0; i < pixels; ++i)
        if (alpha[i] < 0.02f) background_luma.push_back(luma[i]);
    float floor = 0.0f;
    if (!background_luma.empty()) {
        // np.median: the middle value, or the mean of the middle two.
        const std::size_t mid = background_luma.size() / 2;
        std::nth_element(background_luma.begin(), background_luma.begin() + mid,
                         background_luma.end());
        float median = background_luma[mid];
        if (background_luma.size() % 2 == 0) {
            const float below =
                *std::max_element(background_luma.begin(), background_luma.begin() + mid);
            median = (median + below) / 2.0f;
        }
        floor = 0.35f * median;
    }
    std::vector<std::uint8_t> wall_known(pixels), subject_known(pixels);
    for (std::size_t i = 0; i < pixels; ++i) {
        wall_known[i] = alpha[i] < 0.02f && luma[i] > floor;
        subject_known[i] = alpha[i] > 0.95f;
    }
    const std::vector<float> wall = plate_wall ? *plate_wall : fill_plate(frame.linear, w, h, wall_known);
    const std::vector<float> subject = fill_plate(frame.linear, w, h, subject_known);

    std::vector<float> out(pixels);
    for (std::size_t i = 0; i < pixels; ++i) {
        if (!unknown[i]) {
            out[i] = std::clamp(alpha[i], 0.0f, 1.0f);
            continue;
        }
        float sep2 = 0.0f;
        for (int c = 0; c < 3; ++c) {
            const float d = subject[c * pixels + i] - wall[c * pixels + i];
            sep2 += d * d;
        }
        const float confidence = std::clamp((std::sqrt(sep2) - 0.08f) / 0.17f, 0.0f, 1.0f);
        out[i] = std::clamp(confidence * solved[i] + (1.0f - confidence) * alpha[i], 0.0f, 1.0f);
    }
    return out;
}

std::vector<float> MattingStack::subject(const MattingFrame& frame, const std::vector<float>& alpha) {
    const int w = frame.width, h = frame.height;
    const std::size_t pixels = static_cast<std::size_t>(w) * h;
    // The light, interleaved and held to 0..1 as the estimate takes it.
    std::vector<float> image(pixels * 3);
    for (std::size_t i = 0; i < pixels; ++i)
        for (int c = 0; c < 3; ++c)
            image[i * 3 + c] = std::clamp(frame.linear[c * pixels + i], 0.0f, 1.0f);
    const std::vector<float> fg = estimate_foreground_ml(image, alpha, w, h);
    std::vector<float> out(pixels * 3);
    for (std::size_t i = 0; i < pixels; ++i)
        for (int c = 0; c < 3; ++c) out[c * pixels + i] = fg[i * 3 + c] * alpha[i];
    return out;
}

}  // namespace ps::matting

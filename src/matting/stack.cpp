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
            const std::vector<float> alpha = tile_.run(tile_input(frame, x0, y0, tw, th), error);
            if (alpha.empty()) return {};
            // The part of the tile inside the frame; what padding added is
            // dropped.
            const int ch = std::min(th, h - y0), cw = std::min(tw, w - x0);
            for (int y = 0; y < ch; ++y)
                for (int x = 0; x < cw; ++x) {
                    const std::size_t t = static_cast<std::size_t>(y) * tw + x;
                    const std::size_t f = static_cast<std::size_t>(y0 + y) * w + (x0 + x);
                    acc[f] += std::clamp(alpha[t], 0.0f, 1.0f) * feather[t];
                    weight[f] += feather[t];
                }
        }
    }

    // A tile sees part of the subject without the whole, so it is trusted for
    // edge detail and not for what the subject is: only in the coarse
    // matte's soft band, widened, with the band itself feathered so the
    // change from one to the other does not show.
    std::vector<std::uint8_t> soft(pixels);
    for (std::size_t i = 0; i < pixels; ++i) soft[i] = coarse[i] > 0.02f && coarse[i] < 0.98f;
    const std::vector<std::uint8_t> band = dilate_cross(soft, w, h, 24);
    std::vector<float> band_f(pixels);
    for (std::size_t i = 0; i < pixels; ++i) band_f[i] = band[i];
    const std::vector<float> blend = gaussian_blur(band_f, w, h, 8.0f);

    std::vector<float> out(pixels);
    for (std::size_t i = 0; i < pixels; ++i) {
        const float tiles = acc[i] / std::max(weight[i], 1e-6f);
        const float b = std::clamp(blend[i], 0.0f, 1.0f);
        out[i] = b * tiles + (1.0f - b) * coarse[i];
    }
    return out;
}

std::vector<float> MattingStack::band(const MattingFrame& frame, const std::vector<float>& alpha,
                                      std::string* error) {
    const int w = frame.width, h = frame.height;
    const std::size_t pixels = static_cast<std::size_t>(w) * h;

    // The trimap: subject and wall split at half, and unknown wherever the
    // matte is soft, widened.
    std::vector<std::uint8_t> soft(pixels);
    for (std::size_t i = 0; i < pixels; ++i) soft[i] = alpha[i] > 0.03f && alpha[i] < 0.97f;
    const std::vector<std::uint8_t> unknown = dilate_cross(soft, w, h, 10);
    std::vector<double> trimap(pixels);
    for (std::size_t i = 0; i < pixels; ++i)
        trimap[i] = unknown[i] ? 0.5 : (alpha[i] >= 0.5f ? 1.0 : 0.0);

    // Solved in padded cells, so the full-resolution problem stays small.
    // A cell with nothing unknown, or without both subject and wall to solve
    // between, keeps the matte as it is.
    constexpr int kCell = 512, kPad = 48;
    std::vector<float> solved = alpha;
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
            const int ya = std::max(y0 - kPad, 0), yb = std::min(y0 + kCell + kPad, h);
            const int xa = std::max(x0 - kPad, 0), xb = std::min(x0 + kCell + kPad, w);
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
            for (int y = 0; y < ch; ++y)
                for (int x = 0; x < cw; ++x)
                    solved[static_cast<std::size_t>(y0 + y) * w + (x0 + x)] = static_cast<float>(
                        a[static_cast<std::size_t>(y0 - ya + y) * rw + (x0 - xa + x)]);
        }
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
    const std::vector<float> wall = fill_plate(frame.linear, w, h, wall_known);
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

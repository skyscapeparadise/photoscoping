#include "matting/plate.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "matting/image_ops.hpp"
#include "matting/png16.hpp"

namespace ps::matting {

namespace {

float srgb_encode(float l) {
    l = std::clamp(l, 0.0f, 1.0f);
    return l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
}

float srgb_decode(float v) {
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

float distance3(const float* a, const float* b, std::size_t pixels, std::size_t i) {
    float d2 = 0.0f;
    for (int c = 0; c < 3; ++c) {
        const float d = a[c * pixels + i] - b[c * pixels + i];
        d2 += d * d;
    }
    return std::sqrt(d2);
}

// Planar `channels`, each `factor` x `factor` block averaged -- with
// `weight`, a weighted mean and the total weight out.
std::vector<float> pool(const std::vector<float>& src, int w, int h, int channels, int factor,
                        const std::vector<float>* weight, std::vector<float>* weight_out,
                        int* ow, int* oh) {
    const int qw = std::max(1, w / factor), qh = std::max(1, h / factor);
    const std::size_t n = static_cast<std::size_t>(w) * h, qn = static_cast<std::size_t>(qw) * qh;
    std::vector<float> out(qn * channels, 0.0f), total(qn, 0.0f);
    for (int y = 0; y < qh * factor; ++y)
        for (int x = 0; x < qw * factor; ++x) {
            const std::size_t s = static_cast<std::size_t>(y) * w + x;
            const std::size_t d = static_cast<std::size_t>(y / factor) * qw + x / factor;
            const float k = weight ? (*weight)[s] : 1.0f;
            total[d] += k;
            for (int c = 0; c < channels; ++c) out[c * qn + d] += src[c * n + s] * k;
        }
    for (std::size_t d = 0; d < qn; ++d)
        for (int c = 0; c < channels; ++c) out[c * qn + d] /= std::max(total[d], 1e-6f);
    if (weight_out) *weight_out = std::move(total);
    *ow = qw;
    *oh = qh;
    return out;
}

// 4-connected flood from every `seed` through `passable`.
std::vector<std::uint8_t> flood(const std::vector<std::uint8_t>& seed,
                                const std::vector<std::uint8_t>& passable, int w, int h) {
    std::vector<std::uint8_t> reached(seed.size(), 0);
    std::vector<int> stack;
    for (std::size_t i = 0; i < seed.size(); ++i)
        if (seed[i]) {
            reached[i] = 1;
            stack.push_back(static_cast<int>(i));
        }
    while (!stack.empty()) {
        const int i = stack.back();
        stack.pop_back();
        const int x = i % w, y = i / w;
        const int next[4] = {x > 0 ? i - 1 : -1, x + 1 < w ? i + 1 : -1, y > 0 ? i - w : -1,
                             y + 1 < h ? i + w : -1};
        for (int j : next)
            if (j >= 0 && !reached[j] && passable[j]) {
                reached[j] = 1;
                stack.push_back(j);
            }
    }
    return reached;
}

// Closing by a cross: gaps narrower than `steps` fill, regions keep their
// shape.
std::vector<std::uint8_t> close_mask(const std::vector<std::uint8_t>& mask, int w, int h, int steps) {
    std::vector<std::uint8_t> grown = dilate_cross(mask, w, h, steps);
    for (auto& v : grown) v = !v;
    std::vector<std::uint8_t> closed = dilate_cross(grown, w, h, steps);
    for (auto& v : closed) v = !v;
    return closed;
}

// Opening by a cross: specks thinner than `steps` go, regions keep their
// shape.
std::vector<std::uint8_t> open_mask(const std::vector<std::uint8_t>& mask, int w, int h, int steps) {
    std::vector<std::uint8_t> inverse(mask.size());
    for (std::size_t i = 0; i < mask.size(); ++i) inverse[i] = !mask[i];
    std::vector<std::uint8_t> eroded = dilate_cross(inverse, w, h, steps);
    for (auto& v : eroded) v = !v;
    return dilate_cross(eroded, w, h, steps);
}

}  // namespace

void PlateBuilder::add(const MattingFrame& frame) {
    if (count_ == 0) {
        width_ = frame.width;
        height_ = frame.height;
        sum_.assign(frame.linear.size(), 0.0);
    }
    if (frame.width != width_ || frame.height != height_) return;
    for (std::size_t i = 0; i < sum_.size(); ++i) sum_[i] += frame.linear[i];
    ++count_;
}

PlateImage PlateBuilder::finish() const {
    PlateImage plate;
    if (count_ == 0) return plate;
    plate.width = width_;
    plate.height = height_;
    plate.linear.resize(sum_.size());
    for (std::size_t i = 0; i < sum_.size(); ++i) plate.linear[i] = static_cast<float>(sum_[i] / count_);
    return plate;
}

bool save_plate(const std::string& path, const PlateImage& plate) {
    const std::size_t pixels = static_cast<std::size_t>(plate.width) * plate.height;
    std::vector<std::uint16_t> rgba(pixels * 4);
    for (std::size_t i = 0; i < pixels; ++i) {
        for (int c = 0; c < 3; ++c) rgba[i * 4 + c] = to_sample(srgb_encode(plate.linear[c * pixels + i]));
        rgba[i * 4 + 3] = 65535;
    }
    return write_png16_rgba(path, rgba, plate.width, plate.height);
}

bool load_plate(const std::string& path, PlateImage* plate) {
    std::vector<float> planes;
    int w = 0, h = 0, channels = 0;
    if (!read_png16_planes(path, &planes, &w, &h, &channels) || channels < 3) return false;
    const std::size_t pixels = static_cast<std::size_t>(w) * h;
    plate->width = w;
    plate->height = h;
    plate->linear.resize(pixels * 3);
    for (std::size_t i = 0; i < pixels * 3; ++i) plate->linear[i] = srgb_decode(planes[i]);
    return true;
}

PlateImage halve_plate(const PlateImage& plate) {
    PlateImage out;
    out.linear = pool(plate.linear, plate.width, plate.height, 3, 2, nullptr, nullptr, &out.width,
                      &out.height);
    return out;
}

std::vector<float> relight_plate(const PlateImage& plate, const MattingFrame& frame,
                                 const std::vector<float>& alpha, int margin) {
    const int w = frame.width, h = frame.height;
    if (plate.width != w || plate.height != h) return {};
    const std::size_t pixels = static_cast<std::size_t>(w) * h;

    // The wall the frame shows, kept six pixels back from anything the matte
    // gives the subject: an edge's spill is not the wall's light.
    std::vector<std::uint8_t> subject(pixels);
    for (std::size_t i = 0; i < pixels; ++i) subject[i] = alpha[i] >= 0.02f;
    const std::vector<std::uint8_t> near = dilate_cross(subject, w, h, margin);
    std::vector<float> known(pixels);
    std::size_t visible = 0;
    for (std::size_t i = 0; i < pixels; ++i) {
        known[i] = near[i] ? 0.0f : 1.0f;
        visible += !near[i];
    }

    // The light, as frame over plate, at a quarter of the size: the wall's
    // detail is the plate's, and only its light needs finding, which is
    // smooth. Where the frame shows almost no wall there is nothing to
    // measure, and the plate stands as it is.
    std::vector<float> ratio_q;
    int qw = 0, qh = 0;
    if (visible > pixels / 100) {
        std::vector<float> weight;
        const std::vector<float> frame_q = pool(frame.linear, w, h, 3, 4, &known, &weight, &qw, &qh);
        const std::vector<float> plate_q = pool(plate.linear, w, h, 3, 4, &known, nullptr, &qw, &qh);
        const std::size_t qn = static_cast<std::size_t>(qw) * qh;
        std::vector<float> ratio(qn * 3);
        std::vector<std::uint8_t> measured(qn);
        for (std::size_t i = 0; i < qn; ++i) {
            measured[i] = weight[i] >= 8.0f;  // half the block or more is wall
            for (int c = 0; c < 3; ++c)
                ratio[c * qn + i] = std::clamp(frame_q[c * qn + i] / std::max(plate_q[c * qn + i], 1e-3f), 0.0f, 4.0f);
        }
        ratio_q = fill_plate(ratio, qw, qh, measured);
        // A little smoother still: noise, not light, is what varies faster.
        for (int c = 0; c < 3; ++c) {
            std::vector<float> plane(ratio_q.begin() + c * qn, ratio_q.begin() + (c + 1) * qn);
            plane = gaussian_blur(plane, qw, qh, 2.0f);
            std::copy(plane.begin(), plane.end(), ratio_q.begin() + c * qn);
        }
    }

    std::vector<float> wall(pixels * 3);
    if (ratio_q.empty()) {
        wall = plate.linear;
        return wall;
    }
    const std::vector<float> ratio = resize_bilinear_planes(ratio_q, qw, qh, w, h, 3);
    for (std::size_t i = 0; i < pixels * 3; ++i) wall[i] = plate.linear[i] * ratio[i];
    return wall;
}

float plate_tolerance(const MattingFrame& frame, const std::vector<float>& wall,
                      const std::vector<float>& alpha) {
    const std::size_t pixels = static_cast<std::size_t>(frame.width) * frame.height;
    std::vector<float> d;
    // Every 7th pixel is plenty for a median.
    for (std::size_t i = 0; i < pixels; i += 7)
        if (alpha[i] < 0.02f) d.push_back(distance3(frame.linear.data(), wall.data(), pixels, i));
    if (d.size() < 100) return 0.01f;
    std::nth_element(d.begin(), d.begin() + d.size() / 2, d.end());
    return std::max(0.01f, 3.0f * d[d.size() / 2]);
}

std::vector<float> correct_with_plate(const MattingFrame& frame, const std::vector<float>& wall,
                                      const std::vector<float>& alpha, float tolerance) {
    const int w = frame.width, h = frame.height;
    const std::size_t pixels = static_cast<std::size_t>(w) * h;
    constexpr int kBlock = 16;
    const int bw = w / kBlock, bh = h / kBlock;
    if (bw < 4 || bh < 4) return alpha;
    const std::size_t bn = static_cast<std::size_t>(bw) * bh;
    (void)tolerance;

    // Texture: each image's luma less its own 9-pixel local mean -- the
    // plaster's grain, which the plate records in place and which anything
    // in front of the wall hides, whatever its colour. Scale does not
    // matter to it, so a strobe or a shadow leaves it as it is.
    auto luma = [&](const std::vector<float>& planes) {
        std::vector<float> l(pixels);
        for (std::size_t i = 0; i < pixels; ++i)
            l[i] = 0.2126f * planes[i] + 0.7152f * planes[pixels + i] + 0.0722f * planes[2 * pixels + i];
        return l;
    };
    auto highpass = [&](const std::vector<float>& l) {
        // A 9x9 box mean, by running sums across then down.
        std::vector<float> across(pixels), mean(pixels);
        for (int y = 0; y < h; ++y) {
            const float* row = &l[static_cast<std::size_t>(y) * w];
            double run = 0.0;
            for (int x = 0; x < std::min(w, 5); ++x) run += row[x];
            for (int x = 0; x < w; ++x) {
                const int lo = x - 4, hi = x + 4;
                const int n = std::min(hi, w - 1) - std::max(lo, 0) + 1;
                across[static_cast<std::size_t>(y) * w + x] = static_cast<float>(run / n);
                if (hi + 1 < w) run += row[hi + 1];
                if (lo >= 0) run -= row[lo];
            }
        }
        for (int x = 0; x < w; ++x) {
            double run = 0.0;
            for (int y = 0; y < std::min(h, 5); ++y) run += across[static_cast<std::size_t>(y) * w + x];
            for (int y = 0; y < h; ++y) {
                const int lo = y - 4, hi = y + 4;
                const int n = std::min(hi, h - 1) - std::max(lo, 0) + 1;
                mean[static_cast<std::size_t>(y) * w + x] = static_cast<float>(run / n);
                if (hi + 1 < h) run += across[static_cast<std::size_t>(hi + 1) * w + x];
                if (lo >= 0) run -= across[static_cast<std::size_t>(lo) * w + x];
            }
        }
        std::vector<float> out(pixels);
        for (std::size_t i = 0; i < pixels; ++i) out[i] = l[i] - mean[i];
        return out;
    };
    const std::vector<float> frame_hp = highpass(luma(frame.linear));
    const std::vector<float> wall_hp = highpass(luma(wall));

    // Per block: how well the frame's grain matches the plate's (normalised
    // cross-correlation), how much grain the plate has to match, the mean
    // colours, and the mean alpha.
    std::vector<float> ncc(bn), grain(bn), coverage(bn), dist(bn), darker(bn), chroma(bn);
    for (int by = 0; by < bh; ++by)
        for (int bx = 0; bx < bw; ++bx) {
            double sf = 0, sw = 0, sff = 0, sww = 0, sfw = 0, sa = 0;
            double fc[3] = {0, 0, 0}, wc[3] = {0, 0, 0};
            for (int y = by * kBlock; y < (by + 1) * kBlock; ++y)
                for (int x = bx * kBlock; x < (bx + 1) * kBlock; ++x) {
                    const std::size_t i = static_cast<std::size_t>(y) * w + x;
                    const double f = frame_hp[i], g = wall_hp[i];
                    sf += f; sw += g; sff += f * f; sww += g * g; sfw += f * g;
                    sa += alpha[i];
                    for (int c = 0; c < 3; ++c) {
                        fc[c] += frame.linear[c * pixels + i];
                        wc[c] += wall[c * pixels + i];
                    }
                }
            const double n = kBlock * kBlock;
            const double cf = sff - sf * sf / n, cw = sww - sw * sw / n, cfw = sfw - sf * sw / n;
            const std::size_t b = static_cast<std::size_t>(by) * bw + bx;
            ncc[b] = static_cast<float>(cfw / std::sqrt(std::max(cf * cw, 1e-18)));
            grain[b] = static_cast<float>(std::sqrt(std::max(cw, 0.0) / n));
            coverage[b] = static_cast<float>(sa / n);
            double d2 = 0, fl = 0, wl = 0;
            for (int c = 0; c < 3; ++c) {
                fc[c] /= n;
                wc[c] /= n;
                d2 += (fc[c] - wc[c]) * (fc[c] - wc[c]);
                fl += fc[c];
                wl += wc[c];
            }
            dist[b] = static_cast<float>(std::sqrt(d2));
            darker[b] = static_cast<float>(fl / std::max(wl, 1e-4));
            double k2 = 0;
            for (int c = 0; c < 3; ++c) {
                const double k = fc[c] / std::max(fl, 1e-4) - wc[c] / std::max(wl, 1e-4);
                k2 += k * k;
            }
            chroma[b] = static_cast<float>(std::sqrt(k2));
        }
    // Averaged with its neighbours: one block's correlation is noisy, a
    // region's is not.
    std::vector<float> ncc_s(bn), grain_s(bn);
    for (int by = 0; by < bh; ++by)
        for (int bx = 0; bx < bw; ++bx) {
            double sn = 0, sg = 0;
            int count = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int y = by + dy, x = bx + dx;
                    if (y < 0 || x < 0 || y >= bh || x >= bw) continue;
                    sn += ncc[static_cast<std::size_t>(y) * bw + x];
                    sg += grain[static_cast<std::size_t>(y) * bw + x];
                    ++count;
                }
            ncc_s[static_cast<std::size_t>(by) * bw + bx] = static_cast<float>(sn / count);
            grain_s[static_cast<std::size_t>(by) * bw + bx] = static_cast<float>(sg / count);
        }

    // What the wall the frame shows looks like, to measure against: its
    // typical colour distance and grain.
    std::vector<float> wall_dist, wall_grain;
    for (std::size_t b = 0; b < bn; ++b)
        if (coverage[b] < 0.01f) {
            wall_dist.push_back(dist[b]);
            wall_grain.push_back(grain_s[b]);
        }
    if (wall_dist.size() < 50) return alpha;  // too little wall to judge by
    auto median = [](std::vector<float> v) {
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        return v[v.size() / 2];
    };
    const float colour_match = std::max(0.006f, 2.5f * median(wall_dist));
    const float colour_unlike = std::max(0.02f, 5.0f * median(wall_dist));
    const float enough_grain = 0.3f * median(wall_grain);
    // Out of focus, the wall's grain is gone from the frame everywhere, and
    // nothing here can be judged by it.
    std::vector<float> wall_ncc;
    for (std::size_t b = 0; b < bn; ++b)
        if (coverage[b] < 0.01f && grain_s[b] >= enough_grain) wall_ncc.push_back(ncc_s[b]);
    if (wall_ncc.size() < 50 || median(wall_ncc) < 0.15f) return alpha;

    std::vector<std::uint8_t> wall_like(bn), different(bn), visible(bn), border(bn);
    for (std::size_t b = 0; b < bn; ++b) {
        const bool textured = grain_s[b] >= enough_grain;
        const bool shadow = darker[b] > 0.2f && darker[b] < 1.0f && chroma[b] < 0.02f;
        // Wall: its grain where the plate has it, and its colour -- or the
        // wall in shadow.
        wall_like[b] = textured && ncc_s[b] > 0.2f && (dist[b] < colour_match || shadow);
        // Something in front, for certain: a colour no shadow on the wall
        // makes, with the plate's grain gone (where it has any). Grain gone
        // alone is not enough -- the subject's shadow and motion blur hide
        // it too.
        different[b] = dist[b] > colour_unlike && !shadow && !(textured && ncc_s[b] > 0.2f);
        // The wall the frame shows: dropped by the matte, and the plate
        // agreeing -- a hole has no coverage either, and is not wall.
        visible[b] = coverage[b] < 0.02f && !different[b];
        const int x = static_cast<int>(b % bw), y = static_cast<int>(b / bw);
        border[b] = x == 0 || y == 0 || x == bw - 1 || y == bh - 1;
    }

    // The wall the frame shows, as one stretch: what of it reaches the
    // frame's edge. A hole's stray matching blocks are not part of it.
    std::vector<std::uint8_t> border_wall(bn);
    for (std::size_t b = 0; b < bn; ++b) border_wall[b] = border[b] && visible[b];
    const std::vector<std::uint8_t> open_wall = flood(border_wall, visible, bw, bh);
    std::size_t open_count = 0;
    for (auto v : open_wall) open_count += v;
    if (open_count < bn / 50) return alpha;  // a close-up: no wall to stand on

    // Wall kept as subject. An island of subject that is mostly the wall's
    // grain goes whole; wall kept against the subject goes where it matches,
    // its gaps closed so it goes as a region, reached from the open wall.
    std::vector<std::uint8_t> solid(bn), remove(bn, 0);
    for (std::size_t b = 0; b < bn; ++b) solid[b] = coverage[b] >= 0.5f;
    {
        std::vector<int> label(bn, -1);
        for (std::size_t start = 0; start < bn; ++start) {
            if (!solid[start] || label[start] >= 0) continue;
            std::vector<int> members, stack = {static_cast<int>(start)};
            label[start] = static_cast<int>(start);
            int textured = 0, matching = 0;
            while (!stack.empty()) {
                const int i = stack.back();
                stack.pop_back();
                members.push_back(i);
                if (grain_s[i] >= enough_grain) {
                    ++textured;
                    matching += wall_like[i];
                }
                const int x = i % bw, y = i / bw;
                const int next[4] = {x > 0 ? i - 1 : -1, x + 1 < bw ? i + 1 : -1, y > 0 ? i - bw : -1,
                                     y + 1 < bh ? i + bw : -1};
                for (int j : next)
                    if (j >= 0 && solid[j] && label[j] < 0) {
                        label[j] = static_cast<int>(start);
                        stack.push_back(j);
                    }
            }
            if (textured > 0 && matching * 10 >= textured * 6)
                for (int i : members) remove[i] = 1;
        }
    }
    std::vector<std::uint8_t> kept(bn);
    for (std::size_t b = 0; b < bn; ++b) kept[b] = solid[b] && wall_like[b];
    // Closed (gaps filled), then opened (specks dropped).
    kept = close_mask(kept, bw, bh, 1);
    for (std::size_t b = 0; b < bn; ++b) kept[b] = kept[b] && solid[b];
    kept = open_mask(kept, bw, bh, 1);
    std::vector<std::uint8_t> passable(bn);
    for (std::size_t b = 0; b < bn; ++b) passable[b] = kept[b] || open_wall[b];
    const std::vector<std::uint8_t> from_wall = flood(open_wall, passable, bw, bh);
    for (std::size_t b = 0; b < bn; ++b) remove[b] = remove[b] || (kept[b] && from_wall[b]);

    // Holes: dropped, unlike the wall, and closed in -- reaching neither the
    // open wall nor the frame's edge.
    std::vector<std::uint8_t> dropped(bn);
    for (std::size_t b = 0; b < bn; ++b) dropped[b] = coverage[b] < 0.5f && different[b];
    dropped = open_mask(close_mask(dropped, bw, bh, 1), bw, bh, 1);
    for (std::size_t b = 0; b < bn; ++b) dropped[b] = dropped[b] && coverage[b] < 0.5f;
    const std::vector<std::uint8_t> wall_touch = dilate_cross(open_wall, bw, bh, 1);
    std::vector<std::uint8_t> open_seed(bn);
    for (std::size_t b = 0; b < bn; ++b) open_seed[b] = dropped[b] && (border[b] || wall_touch[b]);
    const std::vector<std::uint8_t> open_region = flood(open_seed, dropped, bw, bh);
    std::vector<std::uint8_t> fill(bn);
    for (std::size_t b = 0; b < bn; ++b) fill[b] = dropped[b] && !open_region[b];

    // One block further into the blocks the edges cut through, which the
    // half-covered rule above leaves out: what is removed reaches into the
    // partly covered blocks around it that are not unlike the wall, what is
    // filled into the partly dropped ones that are not like it.
    {
        const std::vector<std::uint8_t> remove_near = dilate_cross(remove, bw, bh, 1);
        const std::vector<std::uint8_t> fill_near = dilate_cross(fill, bw, bh, 1);
        for (std::size_t b = 0; b < bn; ++b) {
            if (remove_near[b] && !remove[b] && coverage[b] > 0.0f && !different[b] && !solid[b]) remove[b] = 1;
            if (fill_near[b] && !fill[b] && coverage[b] < 1.0f && !wall_like[b] && solid[b]) fill[b] = 1;
        }
    }

    // Back to full size, softened over a few pixels so the band refines
    // the new edges rather than taking them as they are.
    std::vector<float> remove_f(bn), fill_f(bn);
    for (std::size_t b = 0; b < bn; ++b) {
        remove_f[b] = remove[b];
        fill_f[b] = fill[b];
    }
    const std::vector<float> remove_up = gaussian_blur(resize_bilinear(remove_f, bw, bh, w, h), w, h, 4.0f);
    const std::vector<float> fill_up = gaussian_blur(resize_bilinear(fill_f, bw, bh, w, h), w, h, 4.0f);
    std::vector<float> out = alpha;
    for (std::size_t i = 0; i < pixels; ++i) {
        out[i] = out[i] * (1.0f - remove_up[i]);
        out[i] = out[i] + (1.0f - out[i]) * fill_up[i];
    }
    return out;
}

}  // namespace ps::matting

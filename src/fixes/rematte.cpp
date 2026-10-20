#include "fixes/rematte.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <tuple>

namespace ps::fixes {

namespace {

// The stack's two models (matting/stack.cpp).
constexpr int kSquare = 2048;
constexpr int kWideW = 2048, kWideH = 1152;

// [a, a + len) moved, then cut, to fit inside [0, limit).
void fit(int* a, int* len, int limit) {
    *len = std::min(*len, limit);
    *a = std::clamp(*a, 0, limit - *len);
}

}  // namespace

PixelRect rematte_rect(const Rematte& box, std::int64_t frame, int frame_w, int frame_h) {
    const std::vector<Point> p = box.at(frame);
    if (p.size() != 2) return {};
    PixelRect r;
    r.x0 = std::clamp(static_cast<int>(std::lround(std::min(p[0].x, p[1].x))), 0, frame_w);
    r.y0 = std::clamp(static_cast<int>(std::lround(std::min(p[0].y, p[1].y))), 0, frame_h);
    r.x1 = std::clamp(static_cast<int>(std::lround(std::max(p[0].x, p[1].x))), 0, frame_w);
    r.y1 = std::clamp(static_cast<int>(std::lround(std::max(p[0].y, p[1].y))), 0, frame_h);
    return r;
}

RematteCrop rematte_crop(const PixelRect& box, int frame_w, int frame_h) {
    RematteCrop c;
    const int margin = std::max(64, static_cast<int>(0.25f * std::max(box.width(), box.height())));
    int w = box.width() + 2 * margin, h = box.height() + 2 * margin;
    c.wide = static_cast<float>(w) / static_cast<float>(h) > 1.33f;
    c.model_w = c.wide ? kWideW : kSquare;
    c.model_h = c.wide ? kWideH : kSquare;
    // To the model's shape, by growing the short side, and no smaller than
    // a quarter of the model.
    const float aspect = static_cast<float>(c.model_w) / static_cast<float>(c.model_h);
    if (static_cast<float>(w) / static_cast<float>(h) < aspect)
        w = static_cast<int>(std::lround(h * aspect));
    else
        h = static_cast<int>(std::lround(w / aspect));
    if (w < c.model_w / 4) {
        w = c.model_w / 4;
        h = c.model_h / 4;
    }
    // Centred on the box, and slid back inside the frame where it runs off;
    // a frame too small for it is cropped whole.
    int x = (box.x0 + box.x1) / 2 - w / 2, y = (box.y0 + box.y1) / 2 - h / 2;
    fit(&x, &w, frame_w);
    fit(&y, &h, frame_h);
    c.crop = {x, y, x + w, y + h};
    c.detail = static_cast<float>(c.model_w) / static_cast<float>(std::max(w, 1));
    return c;
}

int rematte_feather(const PixelRect& box) {
    return std::clamp(static_cast<int>(0.1f * std::min(box.width(), box.height())), 4, 48);
}

float rematte_weight(const PixelRect& box, int x, int y) {
    const int f = rematte_feather(box);
    const int d = std::min({x - box.x0, box.x1 - 1 - x, y - box.y0, box.y1 - 1 - y});
    const float t = std::clamp((static_cast<float>(d) + 0.5f) / static_cast<float>(f), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

std::string patch_name(std::int64_t frame, const PixelRect& box) {
    char name[96];
    std::snprintf(name, sizeof name, "%06lld_%d_%d_%d_%d.png", static_cast<long long>(frame),
                  box.x0, box.y0, box.x1, box.y1);
    return name;
}

std::vector<PatchJob> rematte_jobs(const Fixes& fixes, std::int64_t frame_count, int frame_w,
                                   int frame_h) {
    std::vector<PatchJob> jobs;
    for (const Rematte& box : fixes.rematte) {
        if (box.keys.empty()) continue;
        const std::int64_t first = std::max<std::int64_t>(box.first_frame(), 0);
        const std::int64_t last = std::min<std::int64_t>(box.last_frame(), frame_count - 1);
        for (std::int64_t f = first; f <= last; ++f) {
            const PixelRect r = rematte_rect(box, f, frame_w, frame_h);
            if (r.width() >= kMinRematte && r.height() >= kMinRematte) jobs.push_back({f, r});
        }
    }
    auto key = [](const PatchJob& j) {
        return std::make_tuple(j.frame, j.box.y0, j.box.x0, j.box.y1, j.box.x1);
    };
    std::sort(jobs.begin(), jobs.end(), [&](const PatchJob& a, const PatchJob& b) { return key(a) < key(b); });
    jobs.erase(std::unique(jobs.begin(), jobs.end(),
                           [](const PatchJob& a, const PatchJob& b) {
                               return a.frame == b.frame && a.box == b.box;
                           }),
               jobs.end());
    return jobs;
}

}  // namespace ps::fixes

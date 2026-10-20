#include "fixes/mask.hpp"

#include <algorithm>
#include <cmath>

namespace ps::fixes {

std::vector<std::uint8_t> garbage_mask(const Fixes& fixes, std::int64_t frame, int width,
                                       int height, float scale) {
    if (fixes.garbage.empty() || width <= 0 || height <= 0) return {};

    std::vector<std::vector<Point>> shapes;
    for (const Garbage& g : fixes.garbage) {
        std::vector<Point> pts = g.at(frame);
        for (Point& p : pts) {
            p.x *= scale;
            p.y *= scale;
        }
        if (pts.size() >= 3) shapes.push_back(std::move(pts));
    }

    // Four scanlines a row, and each span's ends counted to the fraction of a
    // pixel they cover: antialiasing across, and four levels of it down.
    constexpr int kSub = 4;
    constexpr float kWeight = 1.0f / kSub;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(width) * height);
    std::vector<float> cover(width + 1);
    std::vector<float> crossings;
    for (int y = 0; y < height; ++y) {
        std::fill(cover.begin(), cover.end(), 0.0f);
        for (int s = 0; s < kSub; ++s) {
            const float sy = static_cast<float>(y) + (static_cast<float>(s) + 0.5f) / kSub;
            for (const auto& pts : shapes) {
                // Even-odd within a shape: where each edge crosses this
                // scanline, paired off left to right.
                crossings.clear();
                for (std::size_t i = 0, n = pts.size(); i < n; ++i) {
                    const Point& a = pts[i];
                    const Point& b = pts[(i + 1) % n];
                    if ((a.y <= sy) == (b.y <= sy)) continue;
                    crossings.push_back(a.x + (sy - a.y) / (b.y - a.y) * (b.x - a.x));
                }
                std::sort(crossings.begin(), crossings.end());
                for (std::size_t i = 0; i + 1 < crossings.size(); i += 2) {
                    const float x0 = std::clamp(crossings[i], 0.0f, static_cast<float>(width));
                    const float x1 = std::clamp(crossings[i + 1], 0.0f, static_cast<float>(width));
                    if (x1 <= x0) continue;
                    const int i0 = static_cast<int>(x0), i1 = static_cast<int>(x1);
                    if (i0 == i1) {
                        cover[i0] += (x1 - x0) * kWeight;
                        continue;
                    }
                    cover[i0] += (static_cast<float>(i0 + 1) - x0) * kWeight;
                    for (int x = i0 + 1; x < i1; ++x) cover[x] += kWeight;
                    cover[i1] += (x1 - static_cast<float>(i1)) * kWeight;
                }
            }
        }
        // Shapes that overlap add past one; the union is held to it.
        std::uint8_t* row = &out[static_cast<std::size_t>(y) * width];
        for (int x = 0; x < width; ++x)
            row[x] = static_cast<std::uint8_t>(std::lround(std::min(cover[x], 1.0f) * 255.0f));
    }
    return out;
}

}  // namespace ps::fixes

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

std::vector<float> stroke_coverage(const Stroke& stroke, std::int64_t frame, int width, int height,
                                   float scale) {
    std::vector<float> cover(static_cast<std::size_t>(width) * height, 0.0f);
    std::vector<Point> pts = stroke.at(frame);
    if (pts.empty()) return cover;
    for (Point& p : pts) {
        p.x *= scale;
        p.y *= scale;
    }
    const float r = std::max(0.5f, stroke.radius * scale);
    const float fade = std::max(0.5f, r * std::clamp(stroke.softness, 0.0f, 1.0f));
    const float solid = r - fade;
    // A lone point is a segment from itself to itself.
    const std::size_t segments = pts.size() == 1 ? 1 : pts.size() - 1;
    for (std::size_t i = 0; i < segments; ++i) {
        const Point& a = pts[i];
        const Point& b = pts.size() == 1 ? pts[0] : pts[i + 1];
        const int x0 = std::max(0, static_cast<int>(std::floor(std::min(a.x, b.x) - r)));
        const int x1 = std::min(width - 1, static_cast<int>(std::ceil(std::max(a.x, b.x) + r)));
        const int y0 = std::max(0, static_cast<int>(std::floor(std::min(a.y, b.y) - r)));
        const int y1 = std::min(height - 1, static_cast<int>(std::ceil(std::max(a.y, b.y) + r)));
        const float dx = b.x - a.x, dy = b.y - a.y;
        const float len2 = dx * dx + dy * dy;
        for (int y = y0; y <= y1; ++y) {
            const float py = static_cast<float>(y) + 0.5f;
            for (int x = x0; x <= x1; ++x) {
                const float px = static_cast<float>(x) + 0.5f;
                float t = len2 > 0.0f ? ((px - a.x) * dx + (py - a.y) * dy) / len2 : 0.0f;
                t = std::clamp(t, 0.0f, 1.0f);
                const float ex = a.x + t * dx - px, ey = a.y + t * dy - py;
                const float d = std::sqrt(ex * ex + ey * ey);
                if (d >= r) continue;
                // Solid out to r - fade, then smoothly to nothing at r.
                float c = d <= solid ? 1.0f : 1.0f - (d - solid) / fade;
                c = c * c * (3.0f - 2.0f * c);
                float& here = cover[static_cast<std::size_t>(y) * width + x];
                here = std::max(here, c);
            }
        }
    }
    return cover;
}

std::vector<std::uint8_t> fixes_mask(const Fixes& fixes, std::int64_t frame, int width, int height,
                                     float scale) {
    if (width <= 0 || height <= 0) return {};
    const bool any_stroke = std::any_of(fixes.strokes.begin(), fixes.strokes.end(),
                                        [&](const Stroke& s) { return s.live_at(frame); });
    if (fixes.garbage.empty() && !any_stroke) return {};

    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    std::vector<float> m(pixels, 1.0f), k(pixels, 0.0f);
    for (const Stroke& s : fixes.strokes) {
        if (!s.live_at(frame)) continue;
        const std::vector<float> c = stroke_coverage(s, frame, width, height, scale);
        for (std::size_t i = 0; i < pixels; ++i) {
            if (c[i] <= 0.0f) continue;
            m[i] *= 1.0f - c[i];
            k[i] = s.mode == PaintMode::kOut ? k[i] * (1.0f - c[i]) : k[i] * (1.0f - c[i]) + c[i];
        }
    }
    const std::vector<std::uint8_t> g = garbage_mask(fixes, frame, width, height, scale);

    std::vector<std::uint8_t> out(pixels * 4);
    for (std::size_t i = 0; i < pixels; ++i) {
        out[i * 4 + 0] = g.empty() ? 255 : g[i];
        out[i * 4 + 1] = static_cast<std::uint8_t>(std::lround(std::clamp(m[i], 0.0f, 1.0f) * 255.0f));
        out[i * 4 + 2] = static_cast<std::uint8_t>(std::lround(std::clamp(k[i], 0.0f, 1.0f) * 255.0f));
        out[i * 4 + 3] = 255;
    }
    return out;
}

}  // namespace ps::fixes

#include "editor/lines.hpp"

#include <cmath>

namespace ps::editor {

void stroke_polyline(gpu::DrawList& draw, const std::vector<float>& xy, bool closed, float width,
                     gpu::Color color) {
    const std::size_t n = xy.size() / 2;
    if (n < 2) return;
    // The feather is a device pixel, whatever the scale.
    const float feather = 1.0f / draw.scale();
    const float inner = width * 0.5f, outer = inner + feather;
    gpu::Color clear = color;
    clear.a = 0.0f;

    std::vector<float> tris;
    std::vector<gpu::Color> colours;
    auto vertex = [&](float x, float y, const gpu::Color& c) {
        tris.push_back(x);
        tris.push_back(y);
        colours.push_back(c);
    };
    // A strip between two offsets along the segment's normal, as two
    // triangles, each offset with its own colour.
    auto strip = [&](float ax, float ay, float bx, float by, float nx, float ny, float o0,
                     const gpu::Color& c0, float o1, const gpu::Color& c1) {
        vertex(ax + nx * o0, ay + ny * o0, c0);
        vertex(bx + nx * o0, by + ny * o0, c0);
        vertex(ax + nx * o1, ay + ny * o1, c1);
        vertex(ax + nx * o1, ay + ny * o1, c1);
        vertex(bx + nx * o0, by + ny * o0, c0);
        vertex(bx + nx * o1, by + ny * o1, c1);
    };
    const std::size_t segments = closed ? n : n - 1;
    for (std::size_t i = 0; i < segments; ++i) {
        const float ax = xy[i * 2], ay = xy[i * 2 + 1];
        const float bx = xy[((i + 1) % n) * 2], by = xy[((i + 1) % n) * 2 + 1];
        const float dx = bx - ax, dy = by - ay;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-4f) continue;
        const float nx = -dy / len, ny = dx / len;
        strip(ax, ay, bx, by, nx, ny, -outer, clear, -inner, color);
        strip(ax, ay, bx, by, nx, ny, -inner, color, inner, color);
        strip(ax, ay, bx, by, nx, ny, inner, color, outer, clear);
    }
    if (!tris.empty())
        draw.add_triangles(tris.data(), colours.data(), static_cast<int>(colours.size()));
}

}  // namespace ps::editor

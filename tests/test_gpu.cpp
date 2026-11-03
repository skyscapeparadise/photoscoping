// The GPU versions of image steps against the CPU versions they replace, on a
// synthetic frame: a soft-edged subject in front of a lit wall.

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "gpu/compute_ops.hpp"
#include "matting/foreground.hpp"
#include "matting/plate.hpp"
#include "matting/png16.hpp"
#include "matting/preprocess.hpp"

namespace {

int g_failures = 0;
void check(bool ok, const char* what, int line) {
    if (ok) return;
    std::printf("FAIL line %d: %s\n", line, what);
    ++g_failures;
}
#define CHECK(expr) check((expr), #expr, __LINE__)

struct Scene {
    int w = 0, h = 0;
    std::vector<float> plate, linear, alpha;  // planar linear Rec.709; alpha
};

Scene make_scene(int w, int h) {
    Scene s;
    s.w = w;
    s.h = h;
    const std::size_t n = static_cast<std::size_t>(w) * h;
    s.plate.resize(n * 3);
    s.linear.resize(n * 3);
    s.alpha.resize(n);
    unsigned seed = 11;
    auto noise = [&] {
        seed = seed * 1664525u + 1013904223u;
        return float(seed >> 8) / float(1u << 24) - 0.5f;
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * w + x;
            // A subject: a disc with a soft edge and a wisp of hair.
            const float dx = x - w * 0.45f, dy = y - h * 0.55f;
            const float r = std::sqrt(dx * dx + dy * dy);
            float a = std::clamp((h * 0.3f - r) / 6.0f, 0.0f, 1.0f);
            if (std::abs(dx - dy * 0.3f) < 2.0f && dy < 0 && r < h * 0.5f) a = std::max(a, 0.4f);
            s.alpha[i] = a;
            // A warm-white wall with grain, lit brighter on one side.
            const float light = 0.8f + 0.4f * x / w;
            const float wall[3] = {0.82f + 0.02f * noise(), 0.80f + 0.02f * noise(), 0.76f + 0.02f * noise()};
            const float fg[3] = {0.35f + 0.1f * std::sin(x * 0.05f), 0.2f, 0.25f + 0.1f * std::cos(y * 0.07f)};
            for (int c = 0; c < 3; ++c) {
                s.plate[c * n + i] = wall[c];
                s.linear[c * n + i] = a * fg[c] + (1 - a) * wall[c] * light;
            }
        }
    return s;
}

float encode_709(float l) { return std::pow(std::clamp(l, 0.0f, 1.0f), 1.0f / 2.4f); }

// As run_export does it on the CPU.
std::vector<std::uint16_t> cpu_subject(const Scene& s, bool with_plate) {
    const std::size_t n = static_cast<std::size_t>(s.w) * s.h;
    std::vector<float> image(n * 3);
    for (std::size_t i = 0; i < n; ++i)
        for (int c = 0; c < 3; ++c) image[i * 3 + c] = std::clamp(s.linear[c * n + i], 0.0f, 1.0f);
    std::vector<float> fg;
    if (with_plate) {
        ps::matting::PlateImage plate{s.plate, s.w, s.h};
        ps::matting::MattingFrame frame;
        frame.width = s.w;
        frame.height = s.h;
        frame.linear = s.linear;
        const std::vector<float> wall = ps::matting::relight_plate(plate, frame, s.alpha);
        std::vector<float> wall_rgb(n * 3);
        for (std::size_t i = 0; i < n; ++i)
            for (int c = 0; c < 3; ++c) wall_rgb[i * 3 + c] = std::clamp(wall[c * n + i], 0.0f, 1.0f);
        fg = ps::matting::estimate_foreground_known(image, s.alpha, wall_rgb, s.w, s.h);
    } else {
        fg = ps::matting::estimate_foreground_ml(image, s.alpha, s.w, s.h);
    }
    std::vector<std::uint16_t> out(n * 4);
    for (std::size_t i = 0; i < n; ++i) {
        for (int c = 0; c < 3; ++c) out[i * 4 + c] = ps::matting::to_sample(encode_709(fg[i * 3 + c]));
        out[i * 4 + 3] = ps::matting::to_sample(s.alpha[i]);
    }
    return out;
}

void compare(const char* what, const Scene& s, const std::vector<std::uint16_t>& cpu,
             const std::vector<std::uint16_t>& gpu) {
    CHECK(cpu.size() == gpu.size());
    if (cpu.size() != gpu.size()) return;
    const std::size_t n = static_cast<std::size_t>(s.w) * s.h;
    double sum = 0, edge_sum = 0;
    int worst = 0, edge_worst = 0, alpha_worst = 0;
    std::size_t edge_n = 0;
    for (std::size_t i = 0; i < n; ++i) {
        alpha_worst = std::max(alpha_worst, std::abs(int(cpu[i * 4 + 3]) - int(gpu[i * 4 + 3])));
        // Colour only where there is subject to have one.
        if (s.alpha[i] < 0.02f) continue;
        for (int c = 0; c < 3; ++c) {
            const int d = std::abs(int(cpu[i * 4 + c]) - int(gpu[i * 4 + c]));
            sum += d;
            worst = std::max(worst, d);
            if (s.alpha[i] < 0.98f) {
                edge_sum += d;
                edge_worst = std::max(edge_worst, d);
                ++edge_n;
            }
        }
    }
    const double mean = sum / (3.0 * n) / 65535.0, edge_mean = edge_sum / std::max<std::size_t>(edge_n, 1) / 65535.0;
    std::printf("  %s: mean %.5f, edges mean %.5f worst %.4f, alpha worst %d\n", what, mean, edge_mean,
                edge_worst / 65535.0, alpha_worst);
    CHECK(alpha_worst == 0);
    CHECK(mean < 0.002);
    CHECK(edge_mean < 0.01);
    (void)worst;
}

}  // namespace

int main() {
    if (!SDL_Init(SDL_INIT_VIDEO)) std::printf("SDL_Init: %s\n", SDL_GetError());
    SDL_GPUDevice* device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL, false, nullptr);
    if (!device) {
        std::printf("no GPU device (%s); skipped\n", SDL_GetError());
        return 0;
    }
    {
        ps::gpu::SubjectGpu subject(device);
        CHECK(subject.ok());
        const Scene s = make_scene(317, 229);
        std::vector<std::uint16_t> gpu;

        CHECK(subject.set_plate({}, s.w, s.h));
        CHECK(subject.finish_linear(s.linear, s.alpha, s.w, s.h, &gpu));
        compare("subject, no plate", s, cpu_subject(s, false), gpu);

        CHECK(subject.set_plate(s.plate, s.w, s.h));
        CHECK(subject.finish_linear(s.linear, s.alpha, s.w, s.h, &gpu));
        compare("subject, with plate", s, cpu_subject(s, true), gpu);
    }
    SDL_DestroyGPUDevice(device);
    SDL_Quit();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("gpu: all checks passed\n");
    return 0;
}

// The pieces an export is made of: 16-bit PNGs both ways, and the encoder in
// every video format, read back by FFmpeg.

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "matting/foreground.hpp"
#include "matting/plate.hpp"
#include "matting/png16.hpp"
#include "matting/stack.hpp"
#include "media/export_format.hpp"
#include "media/video_decoder.hpp"
#include "media/video_encoder.hpp"

namespace {

int g_failures = 0;

void check(bool ok, const char* what, int line) {
    if (ok) return;
    std::printf("FAIL line %d: %s\n", line, what);
    ++g_failures;
}
#define CHECK(expr) check((expr), #expr, __LINE__)

namespace fs = std::filesystem;
using namespace ps;

fs::path temp(const char* name) { return fs::temp_directory_path() / name; }

void test_png16() {
    // A ramp through every 16-bit value the writer can make, and back.
    const int w = 300, h = 7;
    std::vector<float> values(static_cast<std::size_t>(w) * h);
    for (std::size_t i = 0; i < values.size(); ++i) values[i] = static_cast<float>(i % 1000) / 999.0f;
    const std::string path = temp("photoscoping_test_matte.png").string();
    CHECK(matting::write_png16(path, values, w, h));
    std::vector<float> back;
    int bw = 0, bh = 0;
    CHECK(matting::read_png16(path, &back, &bw, &bh));
    CHECK(bw == w && bh == h && back.size() == values.size());
    float worst = 0.0f;
    for (std::size_t i = 0; i < back.size() && i < values.size(); ++i)
        worst = std::max(worst, std::fabs(back[i] - values[i]));
    CHECK(worst <= 0.5f / 65535.0f + 1e-7f);
    // Not a matte: refused, not misread.
    std::vector<std::uint16_t> rgba(static_cast<std::size_t>(w) * h * 4, 1000);
    CHECK(matting::write_png16_rgba(path, rgba, w, h));
    CHECK(!matting::read_png16(path, &back, &bw, &bh));
    fs::remove(path);
}

void test_encoders() {
    // Small, with a gradient in alpha and colour; every video format writes
    // it, and the file reads back at the size and length written.
    const int w = 64, h = 48, frames = 5;
    std::vector<std::uint16_t> rgba(static_cast<std::size_t>(w) * h * 4), grey(static_cast<std::size_t>(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * w + x;
            const auto v = static_cast<std::uint16_t>(65535 * x / (w - 1));
            rgba[i * 4 + 0] = v;
            rgba[i * 4 + 1] = static_cast<std::uint16_t>(65535 * y / (h - 1));
            rgba[i * 4 + 2] = 30000;
            rgba[i * 4 + 3] = v;
            grey[i] = v;
        }
    for (const media::ExportFormatInfo& info : media::kExportFormats) {
        if (info.format == media::ExportFormat::kPngSequence) continue;
        const std::string path = temp((std::string("photoscoping_test_") + info.id + info.extension).c_str()).string();
        media::VideoEncoder encoder;
        std::string error;
        bool ok = encoder.open(path, info.format, w, h, 24000, 1001, &error);
        for (int f = 0; ok && f < frames; ++f)
            ok = encoder.write(info.subject ? rgba.data() : grey.data(), &error);
        ok = ok && encoder.finish(&error);
        if (!ok) std::printf("  %s: %s\n", info.id, error.c_str());
        CHECK(ok);
        media::VideoDecoder decoder;
        CHECK(decoder.open(path, /*hardware=*/false));
        int read = decoder.seek_frame(0) ? 1 : 0;
        while (decoder.read_next()) ++read;
        CHECK(decoder.width() == w && decoder.height() == h);
        CHECK(decoder.rate_num() == 24000 && decoder.rate_den() == 1001);
        if (read != frames) std::printf("  %s: read %d frames\n", info.id, read);
        CHECK(read == frames);
        fs::remove(path);
    }
}

// A plaster wall: a gentle gradient with a fine random grain, the same
// every time.
matting::PlateImage synthetic_plate(int w, int h) {
    matting::PlateImage p;
    p.width = w;
    p.height = h;
    const std::size_t n = static_cast<std::size_t>(w) * h;
    p.linear.resize(n * 3);
    unsigned seed = 12345;
    for (std::size_t i = 0; i < n; ++i) {
        seed = seed * 1664525u + 1013904223u;
        const float grain = (static_cast<float>(seed >> 8) / 16777216.0f - 0.5f) * 0.04f;
        const float base = 0.5f + 0.1f * static_cast<float>(i % w) / w;
        for (int c = 0; c < 3; ++c) p.linear[c * n + i] = base + grain;
    }
    return p;
}

matting::MattingFrame frame_from(const std::vector<float>& linear, int w, int h) {
    matting::MattingFrame f;
    f.width = w;
    f.height = h;
    f.linear = linear;
    f.planes = linear;  // the plate's tests read light only
    return f;
}

void test_plate() {
    const int w = 256, h = 160;
    const std::size_t n = static_cast<std::size_t>(w) * h;
    const matting::PlateImage plate = synthetic_plate(w, h);

    // Saved and read back to 16 bits.
    const std::string path = temp("photoscoping_test_plate.png").string();
    CHECK(matting::save_plate(path, plate));
    matting::PlateImage back;
    CHECK(matting::load_plate(path, &back) && back.width == w && back.height == h);
    float worst = 0.0f;
    for (std::size_t i = 0; i < back.linear.size(); ++i)
        worst = std::max(worst, std::fabs(back.linear[i] - plate.linear[i]));
    CHECK(worst < 1e-3f);
    fs::remove(path);

    // A frame: the wall at 70% -- a strobe a stop and a half down -- and a
    // dark subject square in the middle. The relit plate is the wall at
    // 70% everywhere, behind the subject too.
    std::vector<float> light(n * 3), alpha(n, 0.0f);
    for (std::size_t i = 0; i < n; ++i) {
        const int x = static_cast<int>(i % w), y = static_cast<int>(i / w);
        const bool inside = x >= 96 && x < 160 && y >= 48 && y < 112;
        alpha[i] = inside ? 1.0f : 0.0f;
        for (int c = 0; c < 3; ++c) light[c * n + i] = inside ? 0.1f : plate.linear[c * n + i] * 0.7f;
    }
    const matting::MattingFrame frame = frame_from(light, w, h);
    const std::vector<float> wall = matting::relight_plate(plate, frame, alpha);
    CHECK(wall.size() == n * 3);
    float off_open = 0.0f, off_behind = 0.0f;
    for (std::size_t i = 0; i < n; ++i) {
        const float want = plate.linear[i] * 0.7f;
        const float d = std::fabs(wall[i] - want);
        if (alpha[i] > 0.5f) off_behind = std::max(off_behind, d);
        else off_open = std::max(off_open, d);
    }
    CHECK(off_open < 0.02f);
    CHECK(off_behind < 0.03f);
    const float tolerance = matting::plate_tolerance(frame, wall, alpha);
    CHECK(tolerance >= 0.01f && tolerance < 0.05f);

    // Foreground with the wall known: a half-covered pixel of a red subject
    // over the wall comes out red, not pink.
    std::vector<float> image(n * 3), fg_alpha(n, 0.0f), wall_rgb(n * 3);
    for (std::size_t i = 0; i < n; ++i) {
        const float a = (i % w) < 128 ? 1.0f : 0.5f;
        fg_alpha[i] = a;
        const float red[3] = {0.8f, 0.1f, 0.1f};
        for (int c = 0; c < 3; ++c) {
            const float b = plate.linear[c * n + i];
            wall_rgb[i * 3 + c] = b;
            image[i * 3 + c] = a * red[c] + (1.0f - a) * b;
        }
    }
    const std::vector<float> fg = matting::estimate_foreground_known(image, fg_alpha, wall_rgb, w, h);
    const std::size_t half = static_cast<std::size_t>(80) * w + 200;  // alpha 0.5
    CHECK(std::fabs(fg[half * 3] - 0.8f) < 0.03f && std::fabs(fg[half * 3 + 1] - 0.1f) < 0.03f);

    // The override: an island of "subject" that is really the wall -- its
    // grain in place -- goes; a shadow on the wall that the matte dropped,
    // closed in by the subject, stays dropped.
    std::vector<float> scene(n * 3), wrong(n, 0.0f);
    for (std::size_t i = 0; i < n; ++i) {
        const int x = static_cast<int>(i % w), y = static_cast<int>(i / w);
        const bool ring = x >= 128 && x < 224 && y >= 32 && y < 128 && !(x >= 160 && x < 192 && y >= 64 && y < 96);
        const bool hole_shadow = x >= 160 && x < 192 && y >= 64 && y < 96;
        const bool island = x >= 32 && x < 80 && y >= 48 && y < 96;
        for (int c = 0; c < 3; ++c) {
            const float wall_here = plate.linear[c * n + i];
            scene[c * n + i] = ring ? 0.15f + 0.05f * c : hole_shadow ? wall_here * 0.6f : wall_here;
        }
        wrong[i] = (ring || island) ? 1.0f : 0.0f;
    }
    const matting::MattingFrame scene_frame = frame_from(scene, w, h);
    const std::vector<float> scene_wall = matting::relight_plate(plate, scene_frame, wrong);
    const float scene_tol = matting::plate_tolerance(scene_frame, scene_wall, wrong);
    const std::vector<float> fixed = matting::correct_with_plate(scene_frame, scene_wall, wrong, scene_tol);
    auto mean = [&](int x0, int y0, int x1, int y1) {
        double s2 = 0;
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) s2 += fixed[static_cast<std::size_t>(y) * w + x];
        return s2 / ((x1 - x0) * (y1 - y0));
    };
    CHECK(mean(40, 56, 72, 88) < 0.1);      // the island of wall is gone
    CHECK(mean(140, 40, 156, 120) > 0.95);  // the subject is not
    CHECK(mean(166, 70, 186, 90) < 0.1);    // the shadow is not filled
}

}  // namespace

int main() {
    test_png16();
    test_encoders();
    test_plate();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("export: all checks passed\n");
    return 0;
}

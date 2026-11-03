// The pieces an export is made of: 16-bit PNGs both ways, and the encoder in
// every video format, read back by FFmpeg.

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "light/light_map.hpp"
#include "matting/foreground.hpp"
#include "matting/image_ops.hpp"
#include "media/exr_writer.hpp"
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
        if (info.sequence()) continue;
        const std::string path = temp((std::string("photoscoping_test_") + info.id + info.extension).c_str()).string();
        media::VideoEncoder encoder;
        std::string error;
        bool ok = encoder.open(path, info.format, w, h, 24000, 1001, &error);
        for (int f = 0; ok && f < frames; ++f)
            ok = encoder.write(info.kind == media::ExportKind::kMatte ? grey.data() : rgba.data(), &error);
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

void test_light_maps() {
    // The homography lands each corner of the output on the crop's corner.
    const std::vector<fixes::Point> quad = {{100, 50}, {420, 80}, {400, 300}, {120, 330}};
    const light::Homography hm = light::quad_homography(quad, 300, 250);
    const double out[4][2] = {{0, 0}, {300, 0}, {300, 250}, {0, 250}};
    for (int k = 0; k < 4; ++k) {
        double x, y;
        hm.map(out[k][0], out[k][1], &x, &y);
        CHECK(std::fabs(x - quad[k].x) < 1e-3 && std::fabs(y - quad[k].y) < 1e-3);
    }
    int w = 0, h = 0;
    light::output_size(quad, 0, &w, &h);
    CHECK(w == 321 && h == 281);
    light::output_size(quad, 160, &w, &h);
    CHECK(std::max(w, h) == 160);

    // Warped square to the frame, a crop is the frame's own pixels.
    const int fw = 64, fh = 48;
    const std::size_t fn = static_cast<std::size_t>(fw) * fh;
    std::vector<float> ramp(fn * 3);
    for (std::size_t i = 0; i < fn; ++i)
        for (int c = 0; c < 3; ++c) ramp[c * fn + i] = static_cast<float>(i % fw) / fw;
    const std::vector<fixes::Point> square = {{8, 8}, {40, 8}, {40, 40}, {8, 40}};
    const std::vector<float> crop = light::warp(ramp, 3, fw, fh, light::quad_homography(square, 32, 32), 32, 32);
    CHECK(std::fabs(crop[5] - ramp[8 * fw + 13]) < 1e-4f);

    // A smooth light -- a quadratic -- is recovered from the backdrop around
    // a hole, even with a dark patch on the wall pulling at it.
    const int sw = 200, sh = 150;
    const std::size_t sn = static_cast<std::size_t>(sw) * sh;
    std::vector<float> wall(sn * 3);
    std::vector<std::uint8_t> hidden(sn, 0);
    auto light_at = [](double x, double y) { return 0.4 + 0.001 * x - 0.0005 * y + 0.000004 * x * x; };
    for (int y = 0; y < sh; ++y)
        for (int x = 0; x < sw; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * sw + x;
            const bool patch = x > 20 && x < 30 && y > 20 && y < 30;
            for (int c = 0; c < 3; ++c) wall[c * sn + i] = patch ? 0.05f : static_cast<float>(light_at(x + 0.5, y + 0.5));
            hidden[i] = x > 80 && x < 140 && y > 40 && y < 110;
        }
    const std::vector<fixes::Point> region = {{70, 30}, {150, 30}, {150, 120}, {70, 120}};
    const light::LightSurface surface = light::fit_light_surface(wall, hidden, sw, sh, region);
    CHECK(surface.terms == 6);
    CHECK(std::fabs(surface.at(0, 110.5, 75.5) - light_at(110.5, 75.5)) < 0.003);

    // Filled: the backdrop that shows is left as it is, and what is hidden
    // comes back from the estimate, matched at its edge.
    const int cw = 40, ch = 30;
    const std::size_t cn = static_cast<std::size_t>(cw) * ch;
    std::vector<float> seen(cn * 3), estimate(cn * 3), alpha(cn, 0.0f);
    for (std::size_t i = 0; i < cn; ++i) {
        const int x = static_cast<int>(i % cw), y = static_cast<int>(i / cw);
        const bool performer = x >= 15 && x < 25 && y >= 8 && y < 22;
        alpha[i] = performer ? 1.0f : 0.0f;
        for (int c = 0; c < 3; ++c) {
            const float truth = 0.5f + 0.005f * x;
            seen[c * cn + i] = performer ? 0.1f : truth;
            estimate[c * cn + i] = truth + 0.02f;  // a little off, everywhere
        }
    }
    const std::vector<float> cover = light::cover_weight(alpha, nullptr, cw, ch);
    CHECK(cover[15 * cw + 20] == 1.0f && cover[2 * cw + 2] < 1e-3f);
    const std::vector<float> filled = light::fill_behind(seen, cover, estimate, cw, ch);
    CHECK(std::fabs(filled[2 * cw + 2] - seen[2 * cw + 2]) < 1e-6f);  // untouched
    const float truth_mid = 0.5f + 0.005f * 20;
    CHECK(std::fabs(filled[15 * cw + 20] - truth_mid) < 0.02f);  // no performer left

    // Photoshop's Hue/Saturation.
    auto adjust = [](float r, float g, float b, fixes::LightCrop crop) {
        std::vector<float> p = {r, g, b};
        light::adjust_hue_saturation(p, 1, 1, crop);
        return p;
    };
    fixes::LightCrop c;
    c.hue = 120.0f;
    std::vector<float> p = adjust(1, 0, 0, c);
    CHECK(p[0] < 0.01f && p[1] > 0.99f && p[2] < 0.01f);  // red to green
    c = {};
    c.saturation = -100.0f;
    p = adjust(1, 0, 0, c);
    CHECK(std::fabs(p[0] - p[1]) < 1e-4f && std::fabs(p[1] - p[2]) < 1e-4f);  // grey
    c = {};
    c.lightness = 100.0f;
    p = adjust(0.3f, 0.2f, 0.1f, c);
    CHECK(p[0] > 0.999f && p[1] > 0.999f && p[2] > 0.999f);  // white
    c = {};
    c.colorize = true;
    c.hue = 240.0f;
    c.saturation = 100.0f;
    p = adjust(0.5f, 0.5f, 0.5f, c);
    CHECK(p[2] > p[0] + 0.5f && p[2] > p[1] + 0.5f);  // grey made blue

    // The EXR: OpenEXR's magic number, its header, and every block.
    std::vector<float> rgb(static_cast<std::size_t>(37) * 21 * 3);
    for (std::size_t i = 0; i < rgb.size(); ++i) rgb[i] = static_cast<float>(i % 97) / 13.0f;  // past 1 too
    const std::string exr = temp("photoscoping_test.exr").string();
    CHECK(media::write_exr_half(exr, rgb, 37, 21));
    std::FILE* f = std::fopen(exr.c_str(), "rb");
    unsigned char head[8] = {};
    CHECK(f && std::fread(head, 1, 8, f) == 8);
    if (f) std::fclose(f);
    CHECK(head[0] == 0x76 && head[1] == 0x2f && head[2] == 0x31 && head[3] == 0x01 && head[4] == 2);
    // And back, to half precision, past 1 as well.
    std::vector<float> back;
    int bw = 0, bh = 0;
    CHECK(media::read_exr_half(exr, &back, &bw, &bh) && bw == 37 && bh == 21 && back.size() == rgb.size());
    float worst = 0.0f;
    for (std::size_t i = 0; i < back.size() && i < rgb.size(); ++i)
        worst = std::max(worst, std::fabs(back[i] - rgb[i]) / std::max(1.0f, std::fabs(rgb[i])));
    CHECK(worst < 1e-3f);
    fs::remove(exr);
}

// The blur against a direct, exact Gaussian: the same when narrow, within a
// couple of percent when wide (box passes), on noise with a hard edge in it.
std::vector<float> reference_blur(const std::vector<float>& src, int w, int h, float sigma) {
    const int r = static_cast<int>(4.0 * sigma + 0.5);
    std::vector<double> k(2 * r + 1);
    double sum = 0;
    for (int i = -r; i <= r; ++i) sum += k[i + r] = std::exp(-0.5 * i * i / (double(sigma) * sigma));
    for (double& v : k) v /= sum;
    auto mirror = [](int i, int n) {
        const int p = 2 * n;
        i %= p;
        if (i < 0) i += p;
        return i < n ? i : p - 1 - i;
    };
    std::vector<float> mid(src.size()), out(src.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            double a = 0;
            for (int i = -r; i <= r; ++i) a += k[i + r] * src[mirror(y + i, h) * w + x];
            mid[y * w + x] = float(a);
        }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            double a = 0;
            for (int i = -r; i <= r; ++i) a += k[i + r] * mid[y * w + mirror(x + i, w)];
            out[y * w + x] = float(a);
        }
    return out;
}

void test_blur() {
    const int w = 301, h = 203;
    std::vector<float> img(w * h);
    unsigned seed = 7;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            seed = seed * 1664525u + 1013904223u;
            img[y * w + x] = (x > w / 3 ? 0.8f : 0.1f) + 0.2f * float(seed >> 8) / float(1u << 24);
        }
    for (float sigma : {0.7f, 1.5f, 2.5f, 3.0f, 8.0f, 24.0f, 60.0f}) {
        const std::vector<float> fast = ps::matting::gaussian_blur(img, w, h, sigma);
        const std::vector<float> ref = reference_blur(img, w, h, sigma);
        float worst = 0;
        for (std::size_t i = 0; i < img.size(); ++i) worst = std::max(worst, std::abs(fast[i] - ref[i]));
        if (sigma < 3.0f) CHECK(worst < 1e-5f);
        else CHECK(worst < 0.02f);
        if (worst >= (sigma < 3.0f ? 1e-5f : 0.02f)) std::printf("  blur sigma %.1f: worst %.5f\n", sigma, worst);
    }
}

}  // namespace

int main() {
    test_blur();
    test_png16();
    test_encoders();
    test_plate();
    test_light_maps();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("export: all checks passed\n");
    return 0;
}

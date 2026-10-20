// The pieces an export is made of: 16-bit PNGs both ways, and the encoder in
// every video format, read back by FFmpeg.

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "matting/png16.hpp"
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

}  // namespace

int main() {
    test_png16();
    test_encoders();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("export: all checks passed\n");
    return 0;
}

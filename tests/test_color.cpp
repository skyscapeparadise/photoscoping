// Color math and .cube parsing. Headless: no window, GPU or FFmpeg.
//
// Runs against Kady's Canon LUTs too when they are on this machine
// (PHOTOSCOPING_CANON_LUTS overrides the path); skipped elsewhere.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "color/color_space.hpp"
#include "color/cube_lut.hpp"
#include "color/ycbcr.hpp"

using namespace ps::color;

namespace {

int g_failures = 0;

void check(bool ok, const char* what, int line) {
    if (!ok) {
        std::printf("FAIL line %d: %s\n", line, what);
        ++g_failures;
    }
}
#define CHECK(expr) check((expr), #expr, __LINE__)

bool near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

void test_gamut_matrices() {
    // BT.2087's published Rec.709 -> Rec.2020 matrix.
    const Mat3 m = to_working(Gamut::kRec709);
    const float expected[9] = {0.6274f, 0.3293f, 0.0433f, 0.0691f, 0.9195f,
                               0.0114f, 0.0164f, 0.0880f, 0.8956f};
    for (int i = 0; i < 9; ++i) CHECK(near(m[i], expected[i], 2e-4f));

    // Every gamut keeps white white: rows of a white-preserving matrix sum to 1.
    for (Gamut g : {Gamut::kRec709, Gamut::kRec2020, Gamut::kDciP3, Gamut::kCinemaGamut}) {
        const auto w = apply_matrix(to_working(g), {1.0f, 1.0f, 1.0f});
        CHECK(near(w[0], 1.0f, 1e-4f) && near(w[1], 1.0f, 1e-4f) && near(w[2], 1.0f, 1e-4f));
    }

    // 2020 -> 709 inverts 709 -> 2020.
    const auto round_trip = apply_matrix(working_to_rec709(), apply_matrix(m, {0.2f, 0.5f, 0.8f}));
    CHECK(near(round_trip[0], 0.2f, 1e-4f) && near(round_trip[1], 0.5f, 1e-4f) &&
          near(round_trip[2], 0.8f, 1e-4f));
}

void test_transfers() {
    // BT.2408: PQ reference white (203 nits) is a 58% signal.
    CHECK(near(decode_channel(Transfer::kPq, 0.5807f), 1.0f, 0.01f));
    CHECK(near(decode_channel(Transfer::kPq, 1.0f), 10000.0f / kReferenceWhiteNits, 0.5f));
    CHECK(decode_channel(Transfer::kPq, 0.0f) == 0.0f);

    // BT.2408: HLG reference white is a 75% signal on a 1000-nit display.
    CHECK(near(decode_channel(Transfer::kHlg, 0.75f), 1.0f, 0.01f));
    const auto grey = hlg_to_working({0.75f, 0.75f, 0.75f});
    CHECK(near(grey[0], 1.0f, 0.01f) && near(grey[1], grey[0], 1e-5f));

    CHECK(near(decode_channel(Transfer::kBt1886, 1.0f), 1.0f, 1e-6f));
    CHECK(near(decode_channel(Transfer::kBt1886, 0.5f), std::pow(0.5f, 2.4f), 1e-6f));
}

void test_ycbcr() {
    // 10-bit limited range: black at 64, white at 940, neutral chroma at 512.
    const auto m = ycbcr_to_rgb(YcbcrMatrix::kBt709, SignalRange::kLimited, 10);
    const auto white = apply_affine(m, {940.0f / 1023, 512.0f / 1023, 512.0f / 1023});
    const auto black = apply_affine(m, {64.0f / 1023, 512.0f / 1023, 512.0f / 1023});
    for (int c = 0; c < 3; ++c) {
        CHECK(near(white[c], 1.0f, 1e-4f));
        CHECK(near(black[c], 0.0f, 1e-4f));
    }

    // Full range: codes map straight through.
    const auto f = ycbcr_to_rgb(YcbcrMatrix::kBt2020, SignalRange::kFull, 10);
    const auto full_white = apply_affine(f, {1.0f, 512.0f / 1023, 512.0f / 1023});
    CHECK(near(full_white[0], 1.0f, 1e-4f) && near(full_white[2], 1.0f, 1e-4f));

    // A saturated 709 red round-trips through its own encoding.
    const float kr = 0.2126f, kb = 0.0722f;
    const float y = kr, cb = -kr / (2 * (1 - kb)), cr = (1 - kr) / (2 * (1 - kr));
    const auto e = ycbcr_to_rgb(YcbcrMatrix::kBt709, SignalRange::kFull, 8);
    const auto red = apply_affine(e, {y, cb + 128.0f / 255, cr + 128.0f / 255});
    CHECK(near(red[0], 1.0f, 1e-3f) && near(red[1], 0.0f, 1e-3f) && near(red[2], 0.0f, 1e-3f));
}

// An identity 3D LUT of size n, as text.
std::string identity_cube(int n, const char* header) {
    std::string s = header;
    s += "LUT_3D_SIZE " + std::to_string(n) + "\n";
    char line[96];
    for (int b = 0; b < n; ++b)
        for (int g = 0; g < n; ++g)
            for (int r = 0; r < n; ++r) {
                std::snprintf(line, sizeof line, "%f %f %f\n", r / float(n - 1),
                              g / float(n - 1), b / float(n - 1));
                s += line;
            }
    return s;
}

void test_cube_parsing() {
    std::string error;
    const auto id = parse_cube(identity_cube(5, "TITLE \"identity\"\n"), "id.cube", &error);
    CHECK(id.has_value());
    if (id) {
        CHECK(id->title == "identity" && id->size == 5 && id->is_3d);
        CHECK(!id->output.has_value() && !id->full_range_in.has_value());
        // Tetrahedral interpolation is exact on a linear table, in every one
        // of the six orderings.
        const float probes[6][3] = {{.7f, .4f, .1f}, {.7f, .1f, .4f}, {.4f, .1f, .7f},
                                    {.1f, .4f, .7f}, {.1f, .7f, .4f}, {.4f, .7f, .1f}};
        for (const auto& p : probes) {
            const auto out = id->sample({p[0], p[1], p[2]});
            CHECK(near(out[0], p[0], 1e-5f) && near(out[1], p[1], 1e-5f) &&
                  near(out[2], p[2], 1e-5f));
        }
        const auto clamped = id->sample({-1.0f, 2.0f, 0.5f});
        CHECK(near(clamped[0], 0.0f, 1e-6f) && near(clamped[1], 1.0f, 1e-6f));
    }

    // A non-linear table: tetrahedral must hit the grid points exactly.
    std::string squared = "LUT_3D_SIZE 3\n";
    for (int b = 0; b < 3; ++b)
        for (int g = 0; g < 3; ++g)
            for (int r = 0; r < 3; ++r) {
                const float v[3] = {r / 2.0f, g / 2.0f, b / 2.0f};
                squared += std::to_string(v[0] * v[0]) + " " + std::to_string(v[1] * v[1]) +
                           " " + std::to_string(v[2] * v[2]) + "\n";
            }
    const auto sq = parse_cube(squared, "sq.cube", &error);
    CHECK(sq.has_value());
    if (sq) {
        const auto mid = sq->sample({0.5f, 0.5f, 1.0f});
        CHECK(near(mid[0], 0.25f, 1e-6f) && near(mid[2], 1.0f, 1e-6f));
    }

    // DOMAIN rescales the input.
    const auto dom = parse_cube("DOMAIN_MIN 0 0 0\nDOMAIN_MAX 2 2 2\n" +
                                    identity_cube(2, "").substr(0),
                                "d.cube", &error);
    CHECK(dom.has_value());
    if (dom) CHECK(near(dom->sample({1.0f, 1.0f, 1.0f})[0], 0.5f, 1e-6f));

    // 1D tables interpolate per channel.
    const auto one = parse_cube("LUT_1D_SIZE 2\n0 0 0\n1 0.5 2\n", "1d.cube", &error);
    CHECK(one.has_value() && !one->is_3d);
    if (one) {
        const auto out = one->sample({0.5f, 0.5f, 0.5f});
        CHECK(near(out[0], 0.5f, 1e-6f) && near(out[1], 0.25f, 1e-6f) &&
              near(out[2], 1.0f, 1e-6f));
    }

    // Malformed files are refused with a reason, never half-loaded.
    CHECK(!parse_cube("LUT_3D_SIZE 2\n0 0 0\n", "short.cube", &error));
    CHECK(error.find("expected 8") != std::string::npos);
    CHECK(!parse_cube("0 0 0\n", "nosize.cube", &error));
    CHECK(!parse_cube("LUT_3D_SIZE 2\n0 0 zero\n", "bad.cube", &error));
    CHECK(!parse_cube("LUT_3D_SIZE 2\nFROBNICATE\n", "kw.cube", &error));
}

void test_canon_header() {
    // Canon's header style, on a tiny table.
    const char* header =
        "# Gamut : Cinema Gamut to BT.2020\n"
        "# Gamma : Canon Log 3 to HLGKnee-Vivid\n"
        "# Range : Full in / Full out\n"
        "# LUT_3D_INPUT_RANGE 0.0 1.0\n";
    std::string error;
    const auto lut = parse_cube(identity_cube(2, header), "anything.cube", &error);
    CHECK(lut.has_value());
    if (!lut) return;
    CHECK(lut->full_range_in.value_or(false));
    CHECK(lut->output.has_value());
    if (lut->output) {
        CHECK(lut->output->gamut == Gamut::kRec2020);
        CHECK(lut->output->transfer == Transfer::kHlg);
        CHECK(lut->output->curve == "HLGKnee-Vivid");
    }

    // No header: the file name alone is enough.
    const auto named = parse_cube(identity_cube(2, ""),
                                  "CinemaGamut_CanonLog3-to-DCIP3_DCI_65_FF_Ver.2.0.cube", &error);
    CHECK(named && named->output && named->output->gamut == Gamut::kDciP3 &&
          named->output->transfer == Transfer::kGamma26 && named->full_range_in.value_or(false));
}

// Kady's real ingest LUTs: every one must load and be recognised.
void test_installed_canon_luts() {
    const char* env = std::getenv("PHOTOSCOPING_CANON_LUTS");
    const std::string home = std::getenv("HOME") ? std::getenv("HOME") : "";
    const std::filesystem::path dir =
        env ? env : home + "/Documents/canon-lut-201911/3dlut/65grid-3dlut/full-to-full-range";
    if (!std::filesystem::is_directory(dir)) {
        std::printf("skip: no Canon LUTs at %s\n", dir.c_str());
        return;
    }
    int loaded = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("CinemaGamut_CanonLog3-to-", 0) != 0) continue;
        std::string error;
        const auto lut = load_cube(entry.path().string(), &error);
        if (!lut) {
            std::printf("FAIL %s: %s\n", name.c_str(), error.c_str());
            ++g_failures;
            continue;
        }
        CHECK(lut->is_3d && lut->size == 65);
        CHECK(lut->full_range_in.value_or(false));
        CHECK(lut->output.has_value());
        if (lut->output)
            std::printf("  %-58s -> %s, %s (%s)\n", name.c_str(), gamut_name(lut->output->gamut),
                        transfer_name(lut->output->transfer), lut->output->curve.c_str());
        ++loaded;
    }
    CHECK(loaded == 8);
}

}  // namespace

int main() {
    test_gamut_matrices();
    test_transfers();
    test_ycbcr();
    test_cube_parsing();
    test_canon_header();
    test_installed_canon_luts();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("color: all checks passed\n");
    return 0;
}

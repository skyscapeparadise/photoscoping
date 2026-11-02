#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <set>
#include <string>

#include "app/editor_app.hpp"
#include "color/cube_lut.hpp"
#include "export/clip_export.hpp"
#include "export/light_export.hpp"
#include "media/export_format.hpp"
#include "fixes/fixes.hpp"
#include "fixes/rematte.hpp"
#include "gpu/gpu_context.hpp"
#include "gpu/ingest_pass.hpp"
#include "matting/matte_model.hpp"
#include "matting/preprocess.hpp"
#include "matting/png16.hpp"
#include "matting/stack.hpp"
#include "media/video_decoder.hpp"

namespace {

struct Options {
    std::string clip;
    std::string lut;
    std::int64_t frame = 0;
    std::string export_path;
    bool probe = false;
    bool pipe = false;
    int bench = 0;
    bool software = false;
    std::string matte;
    std::string matte_clip;
    std::string rematte_clip;
    std::string plate_clip;
    std::string light_maps;
    ps::media::ExportFormat light_format = ps::media::ExportFormat::kLightHevc;
    std::string export_path_out;
    ps::media::ExportFormat export_format = ps::media::ExportFormat::kProRes4444;
    std::int64_t export_first = 0;
    std::int64_t export_last = -1;
    std::string matte_stage = "subject";
    std::string models = "models";
    bool model_cpu = false;
    std::string screenshot;
    std::string mattes = "renders";
    std::string view;
    int window_w = 1440;
    int window_h = 900;
};

void usage() {
    std::printf(
        "usage: photoscoping [clip] [options]\n"
        "\n"
        "  --lut FILE     input LUT (.cube) applied to the clip\n"
        "  --frame N      start on frame N (0-based)\n"
        "  --probe        print working-space values for the frame and exit\n"
        "  --bench N      time decode and ingest over N frames and exit\n"
        "  --export FILE  write the frame's working image (linear Rec.2020) as PFM\n"
        "  --pipe         stream every frame from --frame on to stdout and exit:\n"
        "                 a header line \"PSRAW <w> <h> <frames> <fps>\", then\n"
        "                 each frame as w*h RGBA half floats (linear Rec.2020)\n"
        "  --software     decode on the CPU instead of VideoToolbox\n"
        "  --matte FILE   matte --frame with the native stack and write its alpha as\n"
        "                 raw float32 at the frame's size, for comparing\n"
        "  --matte-stage coarse|tiled|band|subject\n"
        "                 how far through the stack --matte goes (default\n"
        "                 subject, whose output is RGB linear Rec.709, interleaved)\n"
        "  --matte-clip DIR\n"
        "                 matte every frame of the clip with the native stack, each\n"
        "                 as a 16-bit PNG in DIR/<clip>_mattes/ the moment it is done;\n"
        "                 frames already there are skipped, so a stopped run resumes\n"
        "  --rematte-clip DIR\n"
        "                 make every patch the re-matte boxes in DIR/<clip>_fixes.json\n"
        "                 want, as 16-bit PNGs in DIR/<clip>_rematte/; patches already\n"
        "                 there are skipped\n"
        "  --export-clip PATH\n"
        "                 export the matted clip, its fixes applied, to PATH; reads\n"
        "                 its mattes, patches and fixes from --mattes DIR\n"
        "  --export-format prores4444|vp9|png|matte-prores422|matte-prores422lt|\n"
        "                 matte-hevc|matte-h264  (default prores4444; png writes\n"
        "                 a folder of 16-bit RGBA frames)\n"
        "  --export-range A:B\n"
        "                 only frames A to B, inclusive\n"
        "  --plate-clip DIR\n"
        "                 make the clean plate the clip's fixes in DIR name\n"
        "  --light-maps DIR\n"
        "                 export the clip's light maps -- every crop in its fixes -- into\n"
        "                 DIR; reads its renders from --mattes DIR\n"
        "  --light-format light-hevc|light-prores422|light-prores422lt|light-prores4444|\n"
        "                 light-h264|light-vp9|light-png|light-exr  (default light-hevc)\n"
        "  --range A:B    with --matte-clip or --export-clip, only frames A to B\n"
        "  --models DIR   where tools/export_matting_model.py wrote the models\n"
        "                 (default: models)\n"
        "  --model-cpu    run the models on the CPU, not Core ML\n"
        "                 (with --matte, --bench N mattes N frames and times them)\n"
        "  --mattes DIR   where tools/matte_clip.py wrote this clip's renders\n"
        "                 (default: renders)\n"
        "  --view NAME    open on footage, matte, subject or overlay\n"
        "  --window WxH   open the window at this size (default 1440x900)\n"
        "  --screenshot FILE\n"
        "                 render the window once, write it as PNG and exit\n"
        "\n"
        "keys: space play/pause, left/right step a frame, home first frame,\n"
        "      1-4 footage, matte, subject, overlay, tab show or hide the inspector\n");
}

std::optional<Options> parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == "--lut") {
            const char* v = value();
            if (!v) return std::nullopt;
            o.lut = v;
        } else if (a == "--frame") {
            const char* v = value();
            if (!v) return std::nullopt;
            o.frame = std::strtoll(v, nullptr, 10);
        } else if (a == "--export") {
            const char* v = value();
            if (!v) return std::nullopt;
            o.export_path = v;
        } else if (a == "--pipe") {
            o.pipe = true;
        } else if (a == "--probe") {
            o.probe = true;
        } else if (a == "--bench") {
            const char* v = value();
            if (!v) return std::nullopt;
            o.bench = std::atoi(v);
            if (o.bench <= 0) return std::nullopt;
        } else if (a == "--matte") {
            const char* v = value();
            if (!v) return std::nullopt;
            o.matte = v;
        } else if (a == "--matte-clip") {
            const char* v = value();
            if (!v) return std::nullopt;
            o.matte_clip = v;
        } else if (a == "--export-clip") {
            const char* v = value();
            if (!v) return std::nullopt;
            o.export_path_out = v;
        } else if (a == "--export-format") {
            const char* v = value();
            if (!v || !ps::media::parse_export_format(v, &o.export_format)) return std::nullopt;
        } else if (a == "--light-maps") {
            const char* v = value();
            if (!v) return std::nullopt;
            o.light_maps = v;
        } else if (a == "--light-format") {
            const char* v = value();
            if (!v || !ps::media::parse_export_format(v, &o.light_format) ||
                ps::media::export_format_info(o.light_format).kind != ps::media::ExportKind::kLight)
                return std::nullopt;
        } else if (a == "--plate-clip") {
            const char* v = value();
            if (!v) return std::nullopt;
            o.plate_clip = v;
        } else if (a == "--export-range" || a == "--range") {
            const char* v = value();
            long long first = 0, last = 0;
            if (!v || std::sscanf(v, "%lld:%lld", &first, &last) != 2 || first < 0 || last < first)
                return std::nullopt;
            o.export_first = first;
            o.export_last = last;
        } else if (a == "--rematte-clip") {
            const char* v = value();
            if (!v) return std::nullopt;
            o.rematte_clip = v;
        } else if (a == "--matte-stage") {
            const char* v = value();
            const std::string stage = v ? v : "";
            if (stage != "coarse" && stage != "tiled" && stage != "band" && stage != "subject")
                return std::nullopt;
            o.matte_stage = v;
        } else if (a == "--models") {
            const char* v = value();
            if (!v) return std::nullopt;
            o.models = v;
        } else if (a == "--model-cpu") {
            o.model_cpu = true;
        } else if (a == "--software") {
            o.software = true;
        } else if (a == "--mattes") {
            const char* v = value();
            if (!v) return std::nullopt;
            o.mattes = v;
        } else if (a == "--view") {
            const char* v = value();
            if (!v) return std::nullopt;
            o.view = v;
        } else if (a == "--screenshot") {
            const char* v = value();
            if (!v) return std::nullopt;
            o.screenshot = v;
        } else if (a == "--window") {
            const char* v = value();
            if (!v || std::sscanf(v, "%dx%d", &o.window_w, &o.window_h) != 2 ||
                o.window_w <= 0 || o.window_h <= 0)
                return std::nullopt;
        } else if (a == "-h" || a == "--help" || a.rfind("-", 0) == 0 || !o.clip.empty()) {
            return std::nullopt;
        } else {
            o.clip = a;
        }
    }
    if (!o.export_path.empty()) o.probe = true;
    if ((o.probe || o.bench || o.pipe || !o.matte.empty() || !o.matte_clip.empty() ||
         !o.rematte_clip.empty() || !o.export_path_out.empty() || !o.plate_clip.empty() ||
         !o.light_maps.empty()) &&
        o.clip.empty())
        return std::nullopt;
    return o;
}

void describe(const ps::media::VideoDecoder& d, const ps::gpu::InputTransform& t,
              const std::optional<ps::color::CubeLut>& lut, std::FILE* out) {
    const auto& s = d.signal();
    std::fprintf(out, "clip    : %dx%d %s, %.3f fps, %lld frames, %s decode\n", d.width(), d.height(),
                d.codec_name().c_str(), d.frame_rate(), static_cast<long long>(d.frame_count()),
                d.hardware_decoding() ? "VideoToolbox" : "software");
    std::fprintf(out, "tags    : matrix %s%s, range %s%s, primaries %s, transfer %s\n",
                ps::color::matrix_name(s.matrix), s.matrix_tagged ? "" : " (untagged, assumed)",
                ps::color::range_name(s.range), s.range_tagged ? "" : " (untagged, assumed)",
                s.primaries.c_str(), s.transfer.c_str());
    if (lut) {
        std::fprintf(out, "lut     : %d^3%s, input %s\n", lut->size, lut->is_3d ? "" : " (1D)",
                    !lut->full_range_in ? "range not stated"
                    : *lut->full_range_in ? "full range"
                                          : "narrow range");
        for (const auto& c : lut->comments) std::fprintf(out, "          %s\n", c.c_str());
        if (!lut->output)
            std::fprintf(out, "          output not stated in the file; assuming Rec.709 BT.1886\n");
    }
    std::fprintf(out, "decode  : %s -> %s, %s -> linear Rec.2020\n",
                t.use_lut ? "LUT" : "file tags", ps::color::gamut_name(t.gamut),
                ps::color::transfer_name(t.transfer));
}

// Prints the working image at a grid of points, each a 9x9 average so sensor
// noise does not decide the reading. Values are linear Rec.2020 with 1.0 at
// reference white (203 nits for HDR).
void print_probe(const std::vector<float>& rgba, Uint32 w, Uint32 h) {
    std::printf("\nworking space (linear Rec.2020, 1.0 = reference white):\n");
    std::printf("  %-10s %9s %9s %9s %10s\n", "point", "R", "G", "B", "Y nits");
    const float fractions[3] = {0.1f, 0.5f, 0.9f};
    for (float fy : fractions) {
        for (float fx : fractions) {
            const int cx = static_cast<int>(fx * (w - 1)), cy = static_cast<int>(fy * (h - 1));
            double sum[3] = {0, 0, 0};
            int n = 0;
            for (int y = cy - 4; y <= cy + 4; ++y)
                for (int x = cx - 4; x <= cx + 4; ++x) {
                    if (x < 0 || y < 0 || x >= static_cast<int>(w) || y >= static_cast<int>(h))
                        continue;
                    const float* p = &rgba[(static_cast<std::size_t>(y) * w + x) * 4];
                    for (int c = 0; c < 3; ++c) sum[c] += p[c];
                    ++n;
                }
            const double r = sum[0] / n, g = sum[1] / n, b = sum[2] / n;
            const double nits = (0.2627 * r + 0.6780 * g + 0.0593 * b) * 203.0;
            char label[24];
            std::snprintf(label, sizeof label, "%.0f%%,%.0f%%", fx * 100, fy * 100);
            std::printf("  %-10s %9.4f %9.4f %9.4f %10.1f\n", label, r, g, b, nits);
        }
    }
    float lo = 1e30f, hi = -1e30f;
    for (std::size_t i = 0; i < rgba.size(); i += 4) {
        const float y = 0.2627f * rgba[i] + 0.6780f * rgba[i + 1] + 0.0593f * rgba[i + 2];
        lo = std::min(lo, y);
        hi = std::max(hi, y);
    }
    std::printf("  frame luminance: min %.4f, max %.4f (%.1f nits)\n", lo, hi, hi * 203.0f);
}

// Portable float map: the simplest format that carries linear float RGB
// unmodified. Rows are stored bottom to top; a negative scale means little
// endian.
bool write_pfm(const std::string& path, const std::vector<float>& rgba, Uint32 w, Uint32 h) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "PF\n%u %u\n-1.0\n", w, h);
    std::vector<float> row(static_cast<std::size_t>(w) * 3);
    for (Uint32 y = h; y-- > 0;) {
        const float* src = &rgba[static_cast<std::size_t>(y) * w * 4];
        for (Uint32 x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c) row[x * 3 + c] = src[x * 4 + c];
        std::fwrite(row.data(), sizeof(float), row.size(), f);
    }
    return std::fclose(f) == 0;
}

bool write_floats(const std::string& path, const std::vector<float>& data) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(data.data(), sizeof(float), data.size(), f) == data.size();
    return std::fclose(f) == 0 && ok;
}

// One frame through the native matting stack, as far as --matte-stage: the
// working image read back, prepared, and matted. Each stage is timed, since
// how long they take is half of what this is for; the other half is the
// alpha, written for comparison with the Python stack's (tools/onnx_spike.py
// stack, then stack-compare).
bool run_matte(const Options& o, ps::gpu::GpuContext& gpu, ps::gpu::IngestPass& ingest,
               ps::media::VideoDecoder& decoder, const ps::gpu::InputTransform& transform) {
    auto ms_since = [](Uint64 start) { return (SDL_GetTicksNS() - start) / 1e6; };

    Uint64 t = SDL_GetTicksNS();
    ps::matting::MattingStack stack;
    std::string error;
    const auto backend =
        o.model_cpu ? ps::matting::MatteModel::Backend::kCpu : ps::matting::MatteModel::Backend::kGpu;
    if (!stack.load(o.models, backend, ps::matting::default_cache_dir(), &error)) {
        std::fprintf(stderr, "cannot load the models in %s: %s\n", o.models.c_str(), error.c_str());
        return false;
    }
    std::printf("models  : %s, %s, loaded in %.0f ms\n", o.models.c_str(),
                o.model_cpu ? "CPU" : "Core ML GPU", ms_since(t));

    // With --bench, that many frames on from --frame, one after another as a
    // run would take them: the first pays each model's warm-up, and the
    // average leaves it out. The first frame's result is what is written.
    const int frames = std::max(1, o.bench);
    double steady_ms = 0.0;
    for (int n = 0; n < frames; ++n) {
        if (n > 0 && !decoder.read_next()) break;
        const Uint64 frame_start = SDL_GetTicksNS();
        t = frame_start;
        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu.device());
        const bool ingested = ingest.process(cmd, decoder, transform);
        SDL_SubmitGPUCommandBuffer(cmd);
        if (!ingested) return false;
        const std::vector<float> working = ingest.read_working();
        const double read_ms = ms_since(t);

        t = SDL_GetTicksNS();
        const ps::matting::MattingFrame frame = ps::matting::prepare_frame(
            working, static_cast<int>(ingest.width()), static_cast<int>(ingest.height()));
        const double prep_ms = ms_since(t);

        // Each stage in turn, as far as asked for, timed.
        const char* const kStages[] = {"coarse", "tiled", "band", "subject"};
        std::vector<float> alpha, out;
        std::string timings;
        for (const char* stage : kStages) {
            t = SDL_GetTicksNS();
            const std::string name = stage;
            if (name == "coarse") alpha = stack.coarse(frame, &error);
            else if (name == "tiled") alpha = stack.tiled(frame, alpha, &error);
            else if (name == "band") alpha = ps::matting::MattingStack::band(frame, alpha, &error);
            else out = ps::matting::MattingStack::subject(frame, alpha);
            if (alpha.empty()) {
                std::fprintf(stderr, "the %s stage failed: %s\n", stage, error.c_str());
                return false;
            }
            char line[64];
            std::snprintf(line, sizeof line, "%s %.0f ms, ", stage, ms_since(t));
            timings += line;
            if (name == o.matte_stage) break;
        }
        const double frame_ms = ms_since(frame_start);
        if (n > 0) steady_ms += frame_ms;
        std::printf("frame %lld: read back %.0f ms, prepare %.0f ms, %stotal %.0f ms\n",
                    static_cast<long long>(decoder.frame_index()), read_ms, prep_ms,
                    timings.c_str(), frame_ms);
        if (n > 0) continue;

        if (o.matte_stage == "subject") {
            // Interleaved, as the Python stack's arrays are.
            const std::size_t pixels = alpha.size();
            std::vector<float> rgb(pixels * 3);
            for (std::size_t i = 0; i < pixels; ++i)
                for (int c = 0; c < 3; ++c) rgb[i * 3 + c] = out[c * pixels + i];
            out = std::move(rgb);
        } else {
            out = alpha;
        }
        if (!write_floats(o.matte, out)) {
            std::fprintf(stderr, "cannot write %s\n", o.matte.c_str());
            return false;
        }
    }
    if (frames > 1)
        std::printf("matte   : %.2f s/frame after the first, over %d frames\n",
                    steady_ms / 1000.0 / (frames - 1), frames - 1);
    return true;
}

// The whole clip through the native stack, as the app's matting job: each
// frame's matte written as a 16-bit PNG the moment it is done, under a
// temporary name and renamed into place so a reader never sees half of one.
// Frames already there are skipped, so a run that was stopped picks up where
// it left off. Progress goes to stdout, a line a frame, for the log.
//
// With a clean plate set in the clip's fixes, each frame is also refined by
// it (exporting::plate_pass) into a folder of its own, named for the plate
// -- the stack's mattes stay as they are -- and a frame already matted is
// only refined.
bool run_matte_clip(const Options& o, ps::gpu::GpuContext& gpu, ps::gpu::IngestPass& ingest,
                    ps::media::VideoDecoder& decoder, const ps::gpu::InputTransform& transform) {
    namespace fs = std::filesystem;
    const std::string stem = fs::path(o.clip).stem().string();
    const fs::path out = fs::path(o.matte_clip) / (stem + "_mattes");
    std::error_code ec;
    fs::create_directories(out, ec);
    auto frame_png = [](const fs::path& dir, std::int64_t i) {
        char name[32];
        std::snprintf(name, sizeof name, "%06lld.png", static_cast<long long>(i));
        return dir / name;
    };

    ps::fixes::Fixes fixes;
    std::string error;
    if (!fixes.load((fs::path(o.matte_clip) / (stem + "_fixes.json")).string(), &error)) {
        std::fprintf(stderr, "cannot read the fixes: %s\n", error.c_str());
        return false;
    }
    const bool with_plate = fixes.plate.set();
    const fs::path plate_out =
        with_plate ? fs::path(o.matte_clip) / ps::fixes::plate_mattes_name(stem, fixes.plate) : fs::path();
    if (with_plate) fs::create_directories(plate_out, ec);

    const std::int64_t total = decoder.frame_count();
    const std::int64_t range_first = std::clamp<std::int64_t>(o.export_first, 0, std::max<std::int64_t>(total - 1, 0));
    const std::int64_t range_last = o.export_last < 0 ? total - 1 : std::min(o.export_last, total - 1);
    auto needs = [&](std::int64_t i, bool* base, bool* plate) {
        *base = !fs::exists(frame_png(out, i), ec);
        *plate = with_plate && !fs::exists(frame_png(plate_out, i), ec);
        return *base || *plate;
    };
    std::int64_t first = range_first, done = 0;
    bool nb = false, np = false;
    while (first <= range_last && !needs(first, &nb, &np)) ++first;
    for (std::int64_t i = range_first; i <= range_last; ++i) done += needs(i, &nb, &np) ? 0 : 1;
    const std::int64_t count = range_last - range_first + 1;
    std::printf("%s: %lld frames, %lld already matted%s\n", o.clip.c_str(), static_cast<long long>(count),
                static_cast<long long>(done), with_plate ? " and refined by the plate" : "");
    std::fflush(stdout);
    if (done >= count) return true;

    ps::matting::PlateImage plate;
    if (with_plate &&
        !ps::exporting::obtain_plate(o.matte_clip, stem, fixes.plate, gpu, ingest, decoder, transform,
                                     &plate, &error)) {
        std::fprintf(stderr, "cannot make the clean plate: %s\n", error.c_str());
        return false;
    }

    // The models only if a frame needs the stack itself.
    bool any_base = false;
    for (std::int64_t i = first; i <= range_last && !any_base; ++i) {
        needs(i, &nb, &np);
        any_base = nb;
    }
    ps::matting::MattingStack stack;
    if (any_base) {
        const Uint64 load_start = SDL_GetTicksNS();
        const auto backend = o.model_cpu ? ps::matting::MatteModel::Backend::kCpu
                                         : ps::matting::MatteModel::Backend::kGpu;
        if (!stack.load(o.models, backend, ps::matting::default_cache_dir(), &error)) {
            std::fprintf(stderr, "cannot load the models in %s: %s\n", o.models.c_str(), error.c_str());
            return false;
        }
        std::printf("models loaded in %.0f s\n", (SDL_GetTicksNS() - load_start) / 1e9);
        std::fflush(stdout);
    }

    auto write = [&](const fs::path& path, const std::vector<float>& alpha, int w, int h) {
        fs::path partial = path;
        partial.replace_extension(".partial.png");
        if (!ps::matting::write_png16(partial.string(), alpha, w, h)) {
            std::fprintf(stderr, "cannot write %s\n", partial.c_str());
            return false;
        }
        fs::rename(partial, path, ec);
        if (ec) {
            std::fprintf(stderr, "cannot rename %s: %s\n", partial.c_str(), ec.message().c_str());
            return false;
        }
        return true;
    };

    if (!decoder.seek_frame(first)) {
        std::fprintf(stderr, "cannot decode frame %lld\n", static_cast<long long>(first));
        return false;
    }
    const Uint64 started = SDL_GetTicksNS();
    std::int64_t made = 0;
    for (std::int64_t i = first; i <= range_last; ++i) {
        if (i > first && !decoder.read_next()) break;
        bool base_needed = false, plate_needed = false;
        if (!needs(i, &base_needed, &plate_needed)) continue;
        const Uint64 t = SDL_GetTicksNS();

        // Each part timed, for the log: where a slow frame went.
        std::string parts;
        Uint64 mark = t;
        auto lap = [&](const char* what) {
            const Uint64 now = SDL_GetTicksNS();
            char text[48];
            std::snprintf(text, sizeof text, "%s %.1f  ", what, (now - mark) / 1e9);
            parts += text;
            mark = now;
        };
        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu.device());
        const bool ingested = ingest.process(cmd, decoder, transform);
        SDL_SubmitGPUCommandBuffer(cmd);
        if (!ingested) return false;
        const ps::matting::MattingFrame frame = ps::matting::prepare_frame(
            ingest.read_working(), static_cast<int>(ingest.width()),
            static_cast<int>(ingest.height()));
        lap("read");

        std::vector<float> alpha;
        if (base_needed) {
            alpha = stack.coarse(frame, &error);
            lap("coarse");
            if (!alpha.empty()) alpha = stack.tiled(frame, alpha, &error);
            lap("tiled");
            if (!alpha.empty()) alpha = ps::matting::MattingStack::band(frame, alpha, &error);
            lap("band");
            if (alpha.empty()) {
                std::fprintf(stderr, "frame %lld: %s\n", static_cast<long long>(i), error.c_str());
                return false;
            }
            if (!write(frame_png(out, i), alpha, frame.width, frame.height)) return false;
            lap("write");
        } else {
            alpha = ps::exporting::read_matte(o.matte_clip, stem, i, frame.width, frame.height);
            if (alpha.empty()) {
                std::fprintf(stderr, "cannot read the matte for frame %lld\n", static_cast<long long>(i));
                return false;
            }
        }
        if (plate_needed) {
            const std::vector<float> refined =
                ps::exporting::plate_pass(frame, alpha, plate, fixes.plate.correct, &error);
            if (refined.empty()) {
                std::fprintf(stderr, "frame %lld: %s\n", static_cast<long long>(i), error.c_str());
                return false;
            }
            if (!write(frame_png(plate_out, i), refined, frame.width, frame.height)) return false;
            lap("plate");
        }

        ++made;
        ++done;
        const double per = (SDL_GetTicksNS() - started) / 1e9 / static_cast<double>(made);
        std::printf("  %lld/%lld  frame %lld  %.2f s  eta %.1f min  (%s)\n", static_cast<long long>(done),
                    static_cast<long long>(count), static_cast<long long>(i),
                    (SDL_GetTicksNS() - t) / 1e9, per * static_cast<double>(count - done) / 60.0,
                    parts.c_str());
        std::fflush(stdout);
    }
    return done >= count;
}

// The clip's clean plate on its own, as the app's plate job: made from the
// frames its fixes name, so the editor has it to show and to estimate with.
bool run_plate_clip(const Options& o, ps::gpu::GpuContext& gpu, ps::gpu::IngestPass& ingest,
                    ps::media::VideoDecoder& decoder, const ps::gpu::InputTransform& transform) {
    namespace fs = std::filesystem;
    const std::string stem = fs::path(o.clip).stem().string();
    ps::fixes::Fixes fixes;
    std::string error;
    if (!fixes.load((fs::path(o.plate_clip) / (stem + "_fixes.json")).string(), &error)) {
        std::fprintf(stderr, "cannot read the fixes: %s\n", error.c_str());
        return false;
    }
    if (!fixes.plate.set()) {
        std::fprintf(stderr, "the clip has no clean plate set\n");
        return false;
    }
    ps::matting::PlateImage plate;
    if (!ps::exporting::obtain_plate(o.plate_clip, stem, fixes.plate, gpu, ingest, decoder, transform,
                                     &plate, &error)) {
        std::fprintf(stderr, "cannot make the clean plate: %s\n", error.c_str());
        return false;
    }
    return true;
}

// The patches the clip's re-matte boxes want, as the app's re-matte job:
// each frame read, its matte found -- the run's PNG if there is one, the
// coarse model's otherwise -- and each box on it looked at again
// (MattingStack::closer), the box's part written as a 16-bit PNG named for
// the frame and the box. A patch already there is skipped, so a stopped run
// resumes and a box moved on some frames is re-matted only on those.
bool run_rematte_clip(const Options& o, ps::gpu::GpuContext& gpu, ps::gpu::IngestPass& ingest,
                      ps::media::VideoDecoder& decoder, const ps::gpu::InputTransform& transform) {
    namespace fs = std::filesystem;
    const std::string stem = fs::path(o.clip).stem().string();
    const fs::path dir(o.rematte_clip);
    const fs::path out = dir / (stem + "_rematte");
    const fs::path mattes = dir / (stem + "_mattes");
    std::error_code ec;
    fs::create_directories(out, ec);

    ps::fixes::Fixes fixes;
    std::string error;
    if (!fixes.load((dir / (stem + "_fixes.json")).string(), &error)) {
        std::fprintf(stderr, "cannot read the fixes: %s\n", error.c_str());
        return false;
    }
    const int fw = decoder.width(), fh = decoder.height();
    const std::vector<ps::fixes::PatchJob> wanted =
        ps::fixes::rematte_jobs(fixes, decoder.frame_count(), fw, fh);

    // Patches no box wants any more -- left where a box was moved from, or
    // by a box since deleted -- are cleared first. Only patches: a file in
    // here named any other way is not this run's to remove.
    std::set<std::string> keep;
    for (const ps::fixes::PatchJob& j : wanted) keep.insert(ps::fixes::patch_name(j.frame, j.box));
    std::size_t cleared = 0;
    for (fs::directory_iterator it(out, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (it->is_regular_file() && ps::fixes::is_patch_name(name) && !keep.count(name)) {
            std::error_code removed;
            if (fs::remove(it->path(), removed)) ++cleared;
        }
    }
    if (cleared) std::printf("cleared %zu patches no box wants\n", cleared);

    std::vector<ps::fixes::PatchJob> jobs;
    for (const ps::fixes::PatchJob& j : wanted)
        if (!fs::exists(out / ps::fixes::patch_name(j.frame, j.box), ec)) jobs.push_back(j);
    std::printf("%s: %zu patches to make\n", o.clip.c_str(), jobs.size());
    std::fflush(stdout);
    if (jobs.empty()) return true;

    const Uint64 load_start = SDL_GetTicksNS();
    ps::matting::MattingStack stack;
    const auto backend =
        o.model_cpu ? ps::matting::MatteModel::Backend::kCpu : ps::matting::MatteModel::Backend::kGpu;
    if (!stack.load(o.models, backend, ps::matting::default_cache_dir(), &error)) {
        std::fprintf(stderr, "cannot load the models in %s: %s\n", o.models.c_str(), error.c_str());
        return false;
    }
    std::printf("models loaded in %.0f s\n", (SDL_GetTicksNS() - load_start) / 1e9);
    std::fflush(stdout);

    ps::matting::PlateImage plate;
    if (fixes.plate.set() &&
        !ps::exporting::obtain_plate(o.rematte_clip, stem, fixes.plate, gpu, ingest, decoder, transform,
                                     &plate, &error)) {
        std::fprintf(stderr, "cannot make the clean plate: %s\n", error.c_str());
        return false;
    }

    const Uint64 started = SDL_GetTicksNS();
    std::int64_t at = -1;  // the frame the decoder holds
    std::size_t made = 0;
    for (std::size_t n = 0; n < jobs.size();) {
        const std::int64_t frame_index = jobs[n].frame;
        const Uint64 t = SDL_GetTicksNS();
        // The next frame is read on; any other is sought.
        const bool decoded = frame_index == at + 1 && at >= 0 ? decoder.read_next()
                                                              : decoder.seek_frame(frame_index);
        if (!decoded) {
            std::fprintf(stderr, "cannot decode frame %lld\n", static_cast<long long>(frame_index));
            return false;
        }
        at = frame_index;
        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu.device());
        const bool ingested = ingest.process(cmd, decoder, transform);
        SDL_SubmitGPUCommandBuffer(cmd);
        if (!ingested) return false;
        const ps::matting::MattingFrame frame = ps::matting::prepare_frame(
            ingest.read_working(), static_cast<int>(ingest.width()),
            static_cast<int>(ingest.height()));

        // The matte the patch is to sit in: the one on disk, which is what
        // the viewer blends it over, or failing that the coarse model's.
        std::vector<float> base = ps::exporting::read_shown_matte(o.rematte_clip, stem, fixes, frame_index,
                                                                  frame.width, frame.height);
        if (base.empty()) base = stack.coarse(frame, &error);
        if (base.empty()) {
            std::fprintf(stderr, "frame %lld: %s\n", static_cast<long long>(frame_index), error.c_str());
            return false;
        }

        // A plate, if the clip has one, lets the closer look know the wall.
        std::vector<float> wall;
        float tolerance = 0.0f;
        if (!plate.empty()) {
            wall = ps::matting::relight_plate(plate, frame, base);
            tolerance = ps::matting::plate_tolerance(frame, wall, base);
        }

        for (; n < jobs.size() && jobs[n].frame == frame_index; ++n) {
            const ps::fixes::PixelRect& box = jobs[n].box;
            float detail = 1.0f;
            const std::vector<float> patch = ps::exporting::make_patch(
                stack, frame, base, box, &detail, &error, wall.empty() ? nullptr : &wall, tolerance);
            if (patch.empty() ||
                !ps::exporting::write_patch((out / ps::fixes::patch_name(frame_index, box)).string(),
                                            patch, box, &error)) {
                std::fprintf(stderr, "frame %lld: %s\n", static_cast<long long>(frame_index),
                             error.c_str());
                return false;
            }
            ++made;
            const double per = (SDL_GetTicksNS() - started) / 1e9 / static_cast<double>(made);
            std::printf("  %zu/%zu  frame %lld  %dx%d at %.1fx  %.2f s  eta %.1f min\n", made,
                        jobs.size(), static_cast<long long>(frame_index), box.width(), box.height(),
                        detail, (SDL_GetTicksNS() - t) / 1e9,
                        per * static_cast<double>(jobs.size() - made) / 60.0);
            std::fflush(stdout);
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const auto options = parse_args(argc, argv);
    if (!options) {
        usage();
        return 1;
    }

    SDL_SetAppMetadata("photoscoping", PHOTOSCOPING_VERSION, "com.rainmultimedia.photoscoping");
    SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_CREATOR_STRING, "rain multimedia");
    SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_COPYRIGHT_STRING,
                               "copyright © 2026 rain multimedia. all rights reserved.");

    std::optional<ps::color::CubeLut> lut;
    if (!options->lut.empty()) {
        std::string error;
        const Uint64 start = SDL_GetTicksNS();
        lut = ps::color::load_cube(options->lut, &error);
        if (!lut) {
            std::fprintf(stderr, "cannot load %s: %s\n", options->lut.c_str(), error.c_str());
            return 1;
        }
        SDL_Log("loaded %s in %.0f ms", options->lut.c_str(), (SDL_GetTicksNS() - start) / 1e6);
        if (lut->full_range_in && !*lut->full_range_in) {
            std::fprintf(stderr, "%s expects narrow-range input, which is not supported yet\n",
                         options->lut.c_str());
            return 1;
        }
    }

    // The headless modes decode here; the window opens its clip itself, on
    // a worker thread, the same way it opens one dropped on it later.
    const bool headless =
        options->probe || options->bench || options->pipe || !options->matte.empty() ||
        !options->matte_clip.empty() || !options->rematte_clip.empty() ||
        !options->export_path_out.empty() || !options->plate_clip.empty() ||
        !options->light_maps.empty();
    ps::media::VideoDecoder decoder;
    if (headless) {
        if (!decoder.open(options->clip, !options->software)) return 1;
        if (!decoder.seek_frame(options->frame)) {
            std::fprintf(stderr, "cannot decode frame %lld\n",
                         static_cast<long long>(options->frame));
            return 1;
        }
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    ps::gpu::GpuContext gpu;
    if (!gpu.create("photoscoping", options->window_w, options->window_h,
                    ps::gpu::ColorMode::kEDR,
                    /*visible=*/!headless && options->screenshot.empty())) {
        SDL_Quit();
        return 1;
    }
    SDL_Log("photoscoping %s\n%s", PHOTOSCOPING_VERSION, gpu.describe().c_str());

    ps::gpu::IngestPass ingest;
    if (!ingest.create(gpu.device())) {
        SDL_Quit();
        return 1;
    }
    if (lut && !ingest.set_lut(*lut)) {
        SDL_Quit();
        return 1;
    }

    ps::gpu::InputTransform transform;
    if (headless) {
        transform = ps::gpu::transform_for(decoder.signal(), lut ? &*lut : nullptr);
        describe(decoder, transform, lut, options->pipe ? stderr : stdout);
    }

    if (!options->matte_clip.empty()) {
        const int status = run_matte_clip(*options, gpu, ingest, decoder, transform) ? 0 : 1;
        ingest.destroy();
        gpu.destroy();
        SDL_Quit();
        return status;
    }

    if (!options->light_maps.empty()) {
        ps::exporting::LightRequest request;
        request.clip = options->clip;
        request.render_dir = options->mattes;
        request.out_dir = options->light_maps;
        request.format = options->light_format;
        request.models = options->models;
        request.model_cpu = options->model_cpu;
        request.first = options->export_first;
        request.last = options->export_last;
        const int status = ps::exporting::run_light_maps(request, gpu, ingest, decoder, transform) ? 0 : 1;
        ingest.destroy();
        gpu.destroy();
        SDL_Quit();
        return status;
    }

    if (!options->plate_clip.empty()) {
        const int status = run_plate_clip(*options, gpu, ingest, decoder, transform) ? 0 : 1;
        ingest.destroy();
        gpu.destroy();
        SDL_Quit();
        return status;
    }

    if (!options->export_path_out.empty()) {
        ps::exporting::ExportRequest request;
        request.clip = options->clip;
        request.render_dir = options->mattes;
        request.out = options->export_path_out;
        request.format = options->export_format;
        request.models = options->models;
        request.model_cpu = options->model_cpu;
        request.first = options->export_first;
        request.last = options->export_last;
        const int status = ps::exporting::run_export(request, gpu, ingest, decoder, transform) ? 0 : 1;
        ingest.destroy();
        gpu.destroy();
        SDL_Quit();
        return status;
    }

    if (!options->rematte_clip.empty()) {
        const int status = run_rematte_clip(*options, gpu, ingest, decoder, transform) ? 0 : 1;
        ingest.destroy();
        gpu.destroy();
        SDL_Quit();
        return status;
    }

    if (!options->matte.empty()) {
        const int status = run_matte(*options, gpu, ingest, decoder, transform) ? 0 : 1;
        ingest.destroy();
        gpu.destroy();
        SDL_Quit();
        return status;
    }

    if (options->probe) {
        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu.device());
        const bool ok = ingest.process(cmd, decoder, transform);
        SDL_SubmitGPUCommandBuffer(cmd);
        bool wrote = true;
        if (ok) {
            const std::vector<float> working = ingest.read_working();
            print_probe(working, ingest.width(), ingest.height());
            if (!options->export_path.empty()) {
                wrote = write_pfm(options->export_path, working, ingest.width(), ingest.height());
                std::printf("%s %s\n", wrote ? "wrote" : "could not write",
                            options->export_path.c_str());
            }
        }
        ingest.destroy();
        gpu.destroy();
        SDL_Quit();
        return ok && wrote ? 0 : 1;
    }

    if (options->pipe) {
        // Frames go out exactly as the working image holds them, so a tool on
        // the other end sees what the app sees. stdout carries nothing else.
        std::fprintf(stdout, "PSRAW %u %u %lld %.6f\n", decoder.width(), decoder.height(),
                     static_cast<long long>(decoder.frame_count() - decoder.frame_index()),
                     decoder.frame_rate());
        std::int64_t sent = 0;
        bool ok = true;
        do {
            SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu.device());
            ok = ingest.process(cmd, decoder, transform);
            SDL_SubmitGPUCommandBuffer(cmd);
            const std::vector<std::uint16_t> frame = ingest.read_working_raw();
            if (!ok || frame.empty() ||
                std::fwrite(frame.data(), sizeof frame[0], frame.size(), stdout) != frame.size()) {
                ok = false;
                break;
            }
            ++sent;
        } while (decoder.read_next());
        std::fflush(stdout);
        std::fprintf(stderr, "piped %lld frames\n", static_cast<long long>(sent));
        ingest.destroy();
        gpu.destroy();
        SDL_Quit();
        return ok ? 0 : 1;
    }

    if (options->bench) {
        // Decode and ingest back to back, waiting on the GPU each frame so the
        // ingest time is real rather than queued.
        double decode_ms = 0, ingest_ms = 0;
        int frames = 0;
        for (; frames < options->bench; ++frames) {
            const Uint64 t0 = SDL_GetTicksNS();
            if (frames > 0 && !decoder.read_next()) break;
            const Uint64 t1 = SDL_GetTicksNS();
            SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu.device());
            ingest.process(cmd, decoder, transform);
            SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
            SDL_WaitForGPUFences(gpu.device(), true, &fence, 1);
            SDL_ReleaseGPUFence(gpu.device(), fence);
            const Uint64 t2 = SDL_GetTicksNS();
            // The first frame's decode happened at open; only count ingest.
            if (frames > 0) decode_ms += (t1 - t0) / 1e6;
            ingest_ms += (t2 - t1) / 1e6;
        }
        std::printf("\nbench   : %d frames, decode %.1f ms/frame, upload + ingest %.1f ms/frame\n",
                    frames, frames > 1 ? decode_ms / (frames - 1) : 0.0,
                    frames ? ingest_ms / frames : 0.0);
        ingest.destroy();
        gpu.destroy();
        SDL_Quit();
        return 0;
    }

    ps::app::EditorApp::Options app_options;
    app_options.clip = options->clip;
    app_options.frame = options->frame;
    app_options.software_decode = options->software;
    app_options.lut = std::move(lut);
    app_options.lut_path = options->lut;
    app_options.render_dir = options->mattes;
    int status = 0;
    {
        ps::app::EditorApp app;
        if (!app.create(gpu, ingest, std::move(app_options))) {
            status = 1;
        } else if (!options->view.empty() && !app.set_view(options->view)) {
            std::fprintf(stderr, "no %s view for this clip\n", options->view.c_str());
            status = 1;
        } else if (!options->screenshot.empty()) {
            const bool ok = app.screenshot(options->screenshot.c_str());
            std::printf("%s %s\n", ok ? "wrote" : "could not write", options->screenshot.c_str());
            status = ok ? 0 : 1;
        } else {
            app.run();
        }
    }

    ingest.destroy();
    gpu.destroy();
    SDL_Quit();
    return status;
}

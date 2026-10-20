#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>

#include "app/editor_app.hpp"
#include "color/cube_lut.hpp"
#include "gpu/gpu_context.hpp"
#include "gpu/ingest_pass.hpp"
#include "matting/matte_model.hpp"
#include "matting/preprocess.hpp"
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
    if ((o.probe || o.bench || o.pipe || !o.matte.empty()) && o.clip.empty()) return std::nullopt;
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
        options->probe || options->bench || options->pipe || !options->matte.empty();
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
                    /*visible=*/!options->probe && !options->bench && !options->pipe &&
                        options->screenshot.empty())) {
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

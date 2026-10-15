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
    if ((o.probe || o.bench || o.pipe) && o.clip.empty()) return std::nullopt;
    return o;
}

ps::gpu::InputTransform transform_for(const ps::media::SignalInfo& s,
                                      const std::optional<ps::color::CubeLut>& lut) {
    ps::gpu::InputTransform t = ps::gpu::transform_from_tags(s);
    if (lut) {
        t.use_lut = true;
        if (lut->output) {
            t.gamut = lut->output->gamut;
            t.transfer = lut->output->transfer;
        } else {
            // A LUT that does not say what it outputs: the common case is
            // Rec.709 video, and it is stated rather than silently assumed.
            t.gamut = ps::color::Gamut::kRec709;
            t.transfer = ps::color::Transfer::kBt1886;
        }
    }
    return t;
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

// The file's name without its directory or extension, which is what the
// inspector and the title call the clip.
std::string clip_name(const std::string& path) {
    const auto slash = path.find_last_of('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const auto dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) name = name.substr(0, dot);
    return name;
}

ps::editor::ClipInfo clip_info(const std::string& name, const ps::media::VideoDecoder& d,
                               const std::string& lut_path) {
    ps::editor::ClipInfo info;
    info.name = name;
    char text[64];
    std::snprintf(text, sizeof text, "%d \xC3\x97 %d", d.width(), d.height());
    info.rows.emplace_back("size", text);
    std::snprintf(text, sizeof text, "%.3f", d.frame_rate());
    // 24.000 reads as a measurement; 24 reads as a frame rate.
    std::string rate = text;
    rate.erase(rate.find_last_not_of('0') + 1);
    if (rate.back() == '.') rate.pop_back();
    info.rows.emplace_back("rate", rate + " fps");
    info.rows.emplace_back("length", std::to_string(d.frame_count()) + " frames");
    info.rows.emplace_back("codec", d.codec_name());
    info.rows.emplace_back("decode", d.hardware_decoding() ? "videotoolbox" : "software");
    info.rows.emplace_back("input", lut_path.empty() ? "file tags" : clip_name(lut_path));
    // Filled in by the app, which knows what has been matted.
    info.rows.emplace_back("matte", "none");
    return info;
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

    ps::media::VideoDecoder decoder;
    if (!options->clip.empty()) {
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

    const bool have_clip = !options->clip.empty();
    ps::gpu::InputTransform transform;
    if (have_clip) {
        transform = transform_for(decoder.signal(), lut);
        describe(decoder, transform, lut, options->pipe ? stderr : stdout);
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

    ps::app::EditorApp::Clip clip;
    if (have_clip) {
        clip.decoder = &decoder;
        clip.name = clip_name(options->clip);
        clip.transform = [&decoder, &lut] { return transform_for(decoder.signal(), lut); };
        clip.path = options->clip;
        clip.lut_path = options->lut;
        clip.render_dir = options->mattes;
        clip.info = clip_info(clip.name, decoder, options->lut);
    }
    int status = 0;
    {
        ps::app::EditorApp app;
        if (!app.create(gpu, ingest, std::move(clip))) {
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

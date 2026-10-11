#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>

#include "color/cube_lut.hpp"
#include "gpu/display_pass.hpp"
#include "gpu/gpu_context.hpp"
#include "gpu/ingest_pass.hpp"
#include "media/video_decoder.hpp"

namespace {

// The window background, as an sRGB-encoded colour.
constexpr float kBackground[3] = {0.075f, 0.078f, 0.090f};

float srgb_to_linear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

// The SDR swapchain is a plain UNORM format that takes encoded values as-is;
// the extended linear one wants linear light. Same colour, two spellings.
SDL_FColor background_for(ps::gpu::ColorMode mode) {
    if (mode == ps::gpu::ColorMode::kSDR)
        return {kBackground[0], kBackground[1], kBackground[2], 1.0f};
    return {srgb_to_linear(kBackground[0]), srgb_to_linear(kBackground[1]),
            srgb_to_linear(kBackground[2]), 1.0f};
}

struct Options {
    std::string clip;
    std::string lut;
    std::int64_t frame = 0;
    std::string export_path;
    bool probe = false;
    bool pipe = false;
    int bench = 0;
    bool software = false;
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
        "\n"
        "keys: space play/pause, left/right step a frame, home first frame\n");
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

// Without a LUT the file's own tags describe the signal.
ps::gpu::InputTransform transform_from_tags(const ps::media::SignalInfo& s) {
    ps::gpu::InputTransform t;
    t.matrix = s.matrix;
    t.range = s.range;
    t.gamut = s.primaries == "bt2020" ? ps::color::Gamut::kRec2020 : ps::color::Gamut::kRec709;
    if (s.transfer == "smpte2084") t.transfer = ps::color::Transfer::kPq;
    else if (s.transfer == "arib-std-b67") t.transfer = ps::color::Transfer::kHlg;
    else if (s.transfer == "linear") t.transfer = ps::color::Transfer::kLinear;
    else t.transfer = ps::color::Transfer::kBt1886;
    return t;
}

ps::gpu::InputTransform transform_for(const ps::media::SignalInfo& s,
                                      const std::optional<ps::color::CubeLut>& lut) {
    ps::gpu::InputTransform t = transform_from_tags(s);
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

void set_title(SDL_Window* window, const std::string& clip, const ps::media::VideoDecoder& d) {
    const auto slash = clip.find_last_of('/');
    const std::string name = slash == std::string::npos ? clip : clip.substr(slash + 1);
    const std::string title = name + " — frame " + std::to_string(d.frame_index()) +
                              (d.frame_count() > 0 ? " / " + std::to_string(d.frame_count() - 1)
                                                   : "");
    SDL_SetWindowTitle(window, title.c_str());
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
    if (!gpu.create("photoscoping", 1440, 900, ps::gpu::ColorMode::kEDR,
                    /*visible=*/!options->probe && !options->bench && !options->pipe)) {
        SDL_Quit();
        return 1;
    }
    SDL_Log("photoscoping %s\n%s", PHOTOSCOPING_VERSION, gpu.describe().c_str());

    ps::gpu::IngestPass ingest;
    ps::gpu::DisplayPass display;
    if (!ingest.create(gpu.device()) || !display.create(gpu)) {
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
        display.destroy();
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
        display.destroy();
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
        display.destroy();
        gpu.destroy();
        SDL_Quit();
        return 0;
    }

    const SDL_FColor clear = background_for(gpu.mode());
    bool fresh_frame = have_clip;
    bool playing = false;
    std::int64_t target = decoder.frame_index();
    Uint64 next_due = 0;
    if (have_clip) set_title(gpu.window(), options->clip, decoder);

    bool running = true;
    while (running) {
        SDL_Event event;
        // Block for input while paused; while playing, wake for the next frame.
        bool got = false;
        if (playing) {
            const Uint64 now = SDL_GetTicksNS();
            const Sint32 wait = next_due > now ? static_cast<Sint32>((next_due - now) / 1000000) : 0;
            got = SDL_WaitEventTimeout(&event, wait);
        } else {
            got = SDL_WaitEvent(&event);
        }
        if (got) {
            do {
                if (event.type == SDL_EVENT_QUIT) running = false;
                if (event.type == SDL_EVENT_KEY_DOWN && have_clip) {
                    switch (event.key.key) {
                        case SDLK_SPACE:
                            playing = !playing;
                            next_due = SDL_GetTicksNS();
                            break;
                        case SDLK_RIGHT: playing = false; target = decoder.frame_index() + 1; break;
                        case SDLK_LEFT:
                            playing = false;
                            target = std::max<std::int64_t>(0, decoder.frame_index() - 1);
                            break;
                        case SDLK_HOME: playing = false; target = 0; break;
                        default: break;
                    }
                }
            } while (SDL_PollEvent(&event));
        }

        if (playing && SDL_GetTicksNS() >= next_due) {
            target = decoder.frame_index() + 1;
            next_due += static_cast<Uint64>(1e9 / (decoder.frame_rate() > 0 ? decoder.frame_rate() : 24.0));
        }
        if (have_clip && target != decoder.frame_index()) {
            const bool ok = target == decoder.frame_index() + 1 ? decoder.read_next()
                                                                 : decoder.seek_frame(target);
            if (ok) {
                fresh_frame = true;
                set_title(gpu.window(), options->clip, decoder);
            } else {
                playing = false;  // end of clip
            }
            target = decoder.frame_index();
        }

        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu.device());
        if (!cmd) {
            SDL_Log("SDL_AcquireGPUCommandBuffer failed: %s", SDL_GetError());
            break;
        }
        if (fresh_frame) {
            transform = transform_for(decoder.signal(), lut);
            ingest.process(cmd, decoder, transform);
            fresh_frame = false;
        }
        SDL_GPUTexture* swapchain = nullptr;
        Uint32 sw = 0, sh = 0;
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, gpu.window(), &swapchain, &sw, &sh)) {
            SDL_Log("SDL_WaitAndAcquireGPUSwapchainTexture failed: %s", SDL_GetError());
            SDL_CancelGPUCommandBuffer(cmd);
            break;
        }
        // Null without an error when the window is minimized; nothing to draw.
        if (swapchain)
            display.draw(cmd, swapchain, sw, sh, ingest.working(), ingest.width(),
                         ingest.height(), clear);
        SDL_SubmitGPUCommandBuffer(cmd);
    }

    ingest.destroy();
    display.destroy();
    gpu.destroy();
    SDL_Quit();
    return 0;
}

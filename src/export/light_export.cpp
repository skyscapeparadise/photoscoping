#include "export/light_export.hpp"

#include "gpu/compute_ops.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>

#include <nlohmann/json.hpp>

#include "export/clip_export.hpp"
#include "fixes/fixes.hpp"
#include "light/light_map.hpp"
#include "matting/image_ops.hpp"
#include "matting/plate.hpp"
#include "matting/png16.hpp"
#include "media/exr_writer.hpp"
#include "media/video_encoder.hpp"

namespace ps::exporting {

namespace {

namespace fs = std::filesystem;

// A crop's name, safe as part of a file name.
std::string file_safe(const std::string& name) {
    std::string out;
    for (char c : name) out += (c == '/' || c == ':' || c == '\\') ? '-' : c;
    return out;
}

struct Output {
    const fixes::LightCrop* crop = nullptr;
    std::string base;  // "clip light 1"
    fs::path final_path, partial_path;
    int width = 0, height = 0;
    media::VideoEncoder encoder;
};

}  // namespace

bool run_light_maps(const LightRequest& request, gpu::GpuContext& gpu, gpu::IngestPass& ingest,
                    media::VideoDecoder& decoder, const gpu::InputTransform& transform) {
    const std::string stem = fs::path(request.clip).stem().string();
    const media::ExportFormatInfo& info = media::export_format_info(request.format);
    if (info.kind != media::ExportKind::kLight) {
        std::fprintf(stderr, "%s is not a light map format\n", info.id);
        return false;
    }
    const int w = decoder.width(), h = decoder.height();
    const std::size_t pixels = static_cast<std::size_t>(w) * h;
    const std::int64_t total = decoder.frame_count();
    const std::int64_t first = std::clamp<std::int64_t>(request.first, 0, std::max<std::int64_t>(total - 1, 0));
    const std::int64_t last =
        request.last < 0 ? total - 1 : std::clamp<std::int64_t>(request.last, first, total - 1);
    const std::int64_t count = last - first + 1;

    fixes::Fixes fixes;
    std::string error;
    if (!fixes.load((fs::path(request.render_dir) / (stem + "_fixes.json")).string(), &error)) {
        std::fprintf(stderr, "cannot read the fixes: %s\n", error.c_str());
        return false;
    }
    if (fixes.lights.empty()) {
        std::fprintf(stderr, "the clip has no light map crops\n");
        return false;
    }
    const fixes::LightExport settings = fixes.light_export;
    matting::PlateImage plate;
    if (fixes.plate.set() &&
        !obtain_plate(request.render_dir, stem, fixes.plate, gpu, ingest, decoder, transform, &plate,
                      &error)) {
        std::fprintf(stderr, "cannot make the clean plate: %s\n", error.c_str());
        return false;
    }
    // Linear light from a camera log is filled from the plate in the same
    // light: the plate decoded as scene light too.
    matting::PlateImage scene_plate;
    if (fixes.plate.set() && settings.linear && request.scene_from_log &&
        !obtain_scene_plate(request.render_dir, stem, fixes.plate, gpu, ingest, decoder, transform,
                            request.scene, &scene_plate, &error)) {
        std::fprintf(stderr, "cannot make the clean plate as scene light: %s\n", error.c_str());
        return false;
    }
    const matting::PlateImage& light_plate = scene_plate.empty() ? plate : scene_plate;
    // Each crop cut from the frame on the GPU, where the frame already is;
    // only crops come back. The CPU route stays for a device without compute.
    auto light_gpu = std::make_unique<gpu::LightGpu>(gpu.device());
    if (std::getenv("PHOTOSCOPING_CPU")) {
        light_gpu.reset();  // for comparing the two routes
    } else if (!light_gpu->ok() || !light_gpu->set_plate(light_plate.linear, w, h)) {
        std::fprintf(stderr, "the GPU cannot cut the light maps here; on the CPU instead\n");
        light_gpu.reset();
    }
    MatteComposer composer;
    composer.open(request.render_dir, stem, fixes, w, h, total, request.models, request.model_cpu,
                  plate.empty() ? nullptr : &plate);
    if (!composer.check(first, last, &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }

    // Each crop's size, from where it is on the first frame.
    std::vector<std::unique_ptr<Output>> outputs;
    for (const fixes::LightCrop& crop : fixes.lights) {
        auto out = std::make_unique<Output>();
        out->crop = &crop;
        out->base = stem + " " + file_safe(crop.name);
        light::output_size(crop.at(first), settings.size, &out->width, &out->height);
        outputs.push_back(std::move(out));
    }

    const bool exr = request.format == media::ExportFormat::kLightExr;
    const bool any_adjusted = std::any_of(fixes.lights.begin(), fixes.lights.end(),
                                          [](const fixes::LightCrop& c) { return c.adjusted(); });
    // A first pass for the exposure, when the light is to be fitted under
    // white: for video and PNG, and for EXR only when an adjustment needs it
    // brought into range to work on.
    const bool measure = settings.linear && (!exr || any_adjusted);
    const std::int64_t steps = measure ? count * 2 : count;
    std::int64_t step = 0;
    const Uint64 started = SDL_GetTicksNS();
    auto progress = [&](std::int64_t f, Uint64 t, const char* what) {
        ++step;
        const double per = (SDL_GetTicksNS() - started) / 1e9 / static_cast<double>(step);
        std::printf("  %lld/%lld  frame %lld  %s  %.2f s  eta %.1f min\n", static_cast<long long>(step),
                    static_cast<long long>(steps), static_cast<long long>(f), what,
                    (SDL_GetTicksNS() - t) / 1e9, per * static_cast<double>(steps - step) / 60.0);
        std::fflush(stdout);
    };
    // Each frame two ways: as the LUT shows it, which the matte is made from
    // (and the look), and as scene light, for linear light.
    std::vector<float> scene_light;  // planar linear Rec.709, the frame read last
    // `look`: the frame as the LUT shows it is wanted in memory -- for the
    // look's light, or a re-matte patch still to make. Measuring linear light
    // needs only the scene light. On the GPU route the light stays there.
    auto read_frame = [&](std::int64_t f, bool seek,
                          bool look = true) -> std::unique_ptr<matting::MattingFrame> {
        if (seek ? !decoder.seek_frame(f) : !decoder.read_next()) return nullptr;
        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu.device());
        bool ingested = ingest.process(cmd, decoder, transform);
        if (ingested && settings.linear) ingested = ingest.process_again(cmd, request.scene);
        SDL_SubmitGPUCommandBuffer(cmd);
        if (!ingested) return nullptr;
        auto frame = std::make_unique<matting::MattingFrame>();
        if (light_gpu) {
            if (look)
                *frame = matting::prepare_frame(ingest.read_working(), static_cast<int>(ingest.width()),
                                                static_cast<int>(ingest.height()));
            return frame;
        }
        if (look || !settings.linear)
            *frame = matting::prepare_frame(ingest.read_working(), static_cast<int>(ingest.width()),
                                            static_cast<int>(ingest.height()));
        if (settings.linear)
            scene_light = matting::prepare_frame(ingest.read_scene(), static_cast<int>(ingest.width()),
                                                 static_cast<int>(ingest.height()))
                              .linear;
        return frame;
    };
    // The texture the light is cut from on the GPU route.
    auto light_texture = [&] { return settings.linear ? ingest.scene() : ingest.working(); };
    // The light the maps are made of.
    auto light_of = [&](const matting::MattingFrame& frame) -> const std::vector<float>& {
        return settings.linear ? scene_light : frame.linear;
    };

    float scale = 1.0f;
    if (measure) {
        // The brightest light any crop sees on the backdrop, in pairs of
        // pixels so a single hot one does not set it.
        float peak = 0.0f;
        for (std::int64_t f = first; f <= last; ++f) {
            const Uint64 t = SDL_GetTicksNS();
            const auto frame = read_frame(f, f == first, /*look=*/false);
            if (!frame) {
                std::fprintf(stderr, "cannot decode frame %lld\n", static_cast<long long>(f));
                return false;
            }
            const std::vector<float> alpha = read_shown_matte(request.render_dir, stem, fixes, f, w, h);
            if (light_gpu && !light_gpu->begin_frame(light_texture(), alpha, w, h)) {
                std::fprintf(stderr, "frame %lld: the GPU could not take the frame\n", static_cast<long long>(f));
                return false;
            }
            for (const auto& out : outputs) {
                const light::Homography hm = light::quad_homography(out->crop->at(f), out->width, out->height);
                std::vector<float> rgb, a, unused;
                if (light_gpu) {
                    if (!light_gpu->warp(hm.m, light::warp_samples(hm, out->width, out->height), out->width,
                                         out->height, false, &rgb, &a, &unused)) {
                        std::fprintf(stderr, "frame %lld: the GPU could not cut a crop\n", static_cast<long long>(f));
                        return false;
                    }
                } else {
                    rgb = light::warp(light_of(*frame), 3, w, h, hm, out->width, out->height);
                    a = alpha.empty() ? std::vector<float>(static_cast<std::size_t>(out->width) * out->height, 0.0f)
                                      : light::warp(alpha, 1, w, h, hm, out->width, out->height);
                }
                const std::size_t n = static_cast<std::size_t>(out->width) * out->height;
                for (std::size_t i = 0; i + 1 < n; ++i) {
                    if (a[i] >= 0.5f || a[i + 1] >= 0.5f) continue;
                    for (int c = 0; c < 3; ++c)
                        peak = std::max(peak, 0.5f * (rgb[c * n + i] + rgb[c * n + i + 1]));
                }
            }
            progress(f, t, "measuring the light");
        }
        if (peak > 1e-4f) scale = 0.95f / peak;
        std::printf("brightest light %.4f; light maps scaled by %.4f\n", peak, scale);
        std::fflush(stdout);
    }

    // Outputs, under temporary names until they are whole.
    std::error_code ec;
    fs::create_directories(request.out_dir, ec);
    for (const auto& out : outputs) {
        out->final_path = fs::path(request.out_dir) / (out->base + info.extension);
        out->partial_path = fs::path(request.out_dir) / (out->base + ".partial" + info.extension);
        if (info.sequence()) {
            fs::remove_all(out->partial_path, ec);
            fs::create_directories(out->partial_path, ec);
        } else if (!out->encoder.open(out->partial_path.string(), request.format, out->width, out->height,
                                      decoder.rate_num(), decoder.rate_den(), &error)) {
            std::fprintf(stderr, "%s: %s\n", out->base.c_str(), error.c_str());
            return false;
        }
    }
    std::printf("light maps of %s, frames %lld-%lld, %zu crops, as %s (%s%s)\n", request.clip.c_str(),
                static_cast<long long>(first), static_cast<long long>(last), outputs.size(), info.label,
                settings.linear ? "linear light" : "the look",
                settings.fill_shadows ? ", shadows filled" : "");
    std::fflush(stdout);

    for (std::int64_t f = first; f <= last; ++f) {
        const Uint64 t = SDL_GetTicksNS();
        const auto frame = read_frame(f, f == first, !light_gpu || composer.needs_frame(f));
        if (!frame) {
            std::fprintf(stderr, "cannot decode frame %lld\n", static_cast<long long>(f));
            return false;
        }
        const std::vector<float> alpha = composer.compose(f, *frame, &error);
        if (alpha.empty()) {
            std::fprintf(stderr, "frame %lld: %s\n", static_cast<long long>(f), error.c_str());
            return false;
        }

        light::FrameLight fl;
        light::BackdropMask mask;
        if (light_gpu) {
            if (!light_gpu->begin_frame(light_texture(), alpha, w, h)) {
                std::fprintf(stderr, "frame %lld: the GPU could not take the frame\n", static_cast<long long>(f));
                return false;
            }
            mask.build(alpha, w, h, light::backdrop_margin(w, settings.fill_shadows));
        } else {
            fl = light::prepare_frame_light(light_of(*frame), alpha, w, h,
                                            light_plate.empty() ? nullptr : &light_plate.linear,
                                            settings.fill_shadows);
        }

        for (const auto& out : outputs) {
            const int ow = out->width, oh = out->height;
            const std::size_t n = static_cast<std::size_t>(ow) * oh;
            std::vector<float> filled;
            if (light_gpu) {
                // As render_crop, its reads of the frame made on the GPU.
                const std::vector<fixes::Point> corners = out->crop->at(f);
                const light::Homography hm = light::quad_homography(corners, ow, oh);
                const bool with_plate = light_gpu->has_plate();
                const light::FitGrid grid =
                    light::fit_grid(corners, w, h, settings.fill_shadows && with_plate);
                std::vector<float> values;
                light::CropSource src;
                if (!light_gpu->gather(grid.rx0, grid.ry0, grid.stride, grid.columns(), grid.rows(), with_plate,
                                       &values)) {
                    std::fprintf(stderr, "frame %lld: the GPU could not sample the backdrop\n",
                                 static_cast<long long>(f));
                    return false;
                }
                src.surface = light::fit_samples(grid, values, mask.wall_at(grid));
                if (!light_gpu->warp(hm.m, light::warp_samples(hm, ow, oh), ow, oh,
                                     with_plate && src.surface.terms, &src.rgb, &src.alpha, &src.plate)) {
                    std::fprintf(stderr, "frame %lld: the GPU could not cut a crop\n", static_cast<long long>(f));
                    return false;
                }
                filled = light::finish_crop(src, hm, ow, oh, settings.fill_shadows, with_plate);
            } else {
                filled = light::render_crop(fl, out->crop->at(f), ow, oh);
            }
            // Encoded for sRGB -- scaled, for linear light; clipped, for the
            // look -- and adjusted there, as Photoshop adjusts.
            const std::vector<float> encoded =
                light::encode_crop(filled, settings.linear, scale, *out->crop, ow, oh);

            char frame_name[32];
            std::snprintf(frame_name, sizeof frame_name, "_%06lld", static_cast<long long>(f));
            bool ok = true;
            if (exr) {
                // Linear light as it is; adjusted, back out of the range it
                // was worked on in.
                std::vector<float> linear = filled;
                if (out->crop->adjusted() || !settings.linear) {
                    for (std::size_t i = 0; i < n * 3; ++i)
                        linear[i] = light::srgb_decode(encoded[i]) / (settings.linear ? scale : 1.0f);
                }
                ok = media::write_exr_half((out->partial_path / (out->base + frame_name + ".exr")).string(),
                                           linear, ow, oh);
            } else {
                const int channels = info.sequence() ? 3 : 4;
                std::vector<std::uint16_t> samples(n * channels);
                for (std::size_t i = 0; i < n; ++i) {
                    for (int c = 0; c < 3; ++c)
                        samples[i * channels + c] = matting::to_sample(encoded[c * n + i]);
                    if (channels == 4) samples[i * 4 + 3] = 65535;
                }
                if (info.sequence())
                    ok = matting::write_png16_rgb((out->partial_path / (out->base + frame_name + ".png")).string(),
                                                  samples, ow, oh);
                else
                    ok = out->encoder.write(samples.data(), &error);
            }
            if (!ok) {
                std::fprintf(stderr, "frame %lld, %s: cannot write it%s%s\n", static_cast<long long>(f),
                             out->base.c_str(), error.empty() ? "" : ": ", error.c_str());
                return false;
            }
        }
        progress(f, t, "writing");
    }

    for (const auto& out : outputs) {
        if (!info.sequence() && !out->encoder.finish(&error)) {
            std::fprintf(stderr, "%s: %s\n", out->base.c_str(), error.c_str());
            return false;
        }
        // Into place: over a file of the name, or a folder of nothing but
        // an earlier export's frames; never over anything else.
        if (fs::is_directory(out->final_path, ec)) {
            bool ours = true;
            for (fs::directory_iterator it(out->final_path, ec), end; !ec && it != end; it.increment(ec)) {
                const std::string name = it->path().filename().string();
                if (name != ".DS_Store" && name.compare(0, out->base.size() + 1, out->base + "_") != 0) ours = false;
            }
            if (!ours) {
                std::fprintf(stderr, "%s is a folder with other things in it; the light map is at %s\n",
                             out->final_path.c_str(), out->partial_path.c_str());
                return false;
            }
            fs::remove_all(out->final_path, ec);
        } else if (fs::exists(out->final_path, ec)) {
            fs::remove(out->final_path, ec);
        }
        fs::rename(out->partial_path, out->final_path, ec);
        if (ec) {
            std::fprintf(stderr, "cannot rename %s: %s\n", out->partial_path.c_str(), ec.message().c_str());
            return false;
        }

        // How to use it, beside it.
        nlohmann::json side = {
            {"light", out->crop->name},
            {"clip", fs::path(request.clip).filename().string()},
            {"frames", {first, last}},
            {"frame_rate", {decoder.rate_num(), decoder.rate_den()}},
            {"size", {out->width, out->height}},
            {"contents", settings.linear ? "linear light" : "the look"},
            {"decoded", settings.linear ? (request.scene_from_log ? request.scene_name
                                                                  : std::string("by the file's tags"))
                                        : std::string("through the LUT")},
            {"primaries", "Rec.709"},
            {"encoding", exr ? "linear" : "sRGB"},
            {"shadows", settings.fill_shadows ? "filled" : "kept"},
            {"emission_strength", exr ? 1.0f : (settings.linear ? 1.0f / scale : 1.0f)},
            {"note", exr ? "Linear light as it fell; use as it is."
                         : settings.linear ? "Scaled to fit under white: set the emission strength to "
                                             "emission_strength to restore the light's own level."
                                           : "The look as the viewer shows it, clipped at white."},
        };
        std::ofstream((fs::path(request.out_dir) / (out->base + ".json")).string()) << side.dump(2) << "\n";
        std::printf("exported %s\n", out->final_path.c_str());
    }
    return true;
}

}  // namespace ps::exporting

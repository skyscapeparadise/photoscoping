#include "export/clip_export.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>

#include "fixes/fixes.hpp"
#include "fixes/mask.hpp"
#include "matting/foreground.hpp"
#include "matting/matte_model.hpp"
#include "matting/png16.hpp"
#include "media/video_encoder.hpp"

namespace ps::exporting {

namespace {

namespace fs = std::filesystem;

std::string frame_name(std::int64_t frame) {
    char name[16];
    std::snprintf(name, sizeof name, "%06lld.png", static_cast<long long>(frame));
    return name;
}

// Linear light to Rec.709 video: the inverse of BT.1886's 2.4, which is how
// the footage was read in -- so where the subject is solid, its pixels come
// out as they went in.
float encode_709(float linear) {
    return std::pow(std::clamp(linear, 0.0f, 1.0f), 1.0f / 2.4f);
}

// "out.mov" -> "out.partial.mov"; "out" -> "out.partial".
fs::path partial_of(const fs::path& out) {
    fs::path p = out;
    const std::string ext = out.extension().string();
    p.replace_extension();
    return p.string() + ".partial" + ext;
}

// Whether a folder holds nothing but a PNG sequence an export named `name`
// would have written into it: <name>_NNNNNN.png.
bool only_frames_of(const fs::path& dir, const std::string& name_of_export) {
    const std::string prefix = name_of_export + "_";
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name == ".DS_Store") continue;
        if (!it->is_regular_file() || name.size() != prefix.size() + 10 ||
            name.compare(0, prefix.size(), prefix) != 0 ||
            name.compare(name.size() - 4, 4, ".png") != 0)
            return false;
    }
    return !ec;
}

}  // namespace

std::vector<float> make_patch(matting::MattingStack& stack, const matting::MattingFrame& frame,
                              const std::vector<float>& base, const fixes::PixelRect& box,
                              float* detail, std::string* error, const std::vector<float>* wall,
                              float tolerance) {
    const fixes::RematteCrop c = fixes::rematte_crop(box, frame.width, frame.height);
    if (detail) *detail = c.detail;
    const std::vector<float> alpha = stack.closer(frame, base, c.crop.x0, c.crop.y0, c.crop.width(),
                                                  c.crop.height(), c.wide, error, wall, tolerance);
    if (alpha.empty()) return {};
    std::vector<float> patch(static_cast<std::size_t>(box.width()) * box.height());
    for (int y = 0; y < box.height(); ++y)
        for (int x = 0; x < box.width(); ++x)
            patch[static_cast<std::size_t>(y) * box.width() + x] =
                alpha[static_cast<std::size_t>(box.y0 - c.crop.y0 + y) * c.crop.width() +
                      (box.x0 - c.crop.x0 + x)];
    return patch;
}

bool write_patch(const std::string& path, const std::vector<float>& patch,
                 const fixes::PixelRect& box, std::string* error) {
    fs::path partial = path;
    partial.replace_extension(".partial.png");
    if (!matting::write_png16(partial.string(), patch, box.width(), box.height())) {
        *error = "cannot write " + partial.string();
        return false;
    }
    std::error_code ec;
    fs::rename(partial, path, ec);
    if (ec) {
        *error = "cannot rename " + partial.string() + ": " + ec.message();
        return false;
    }
    return true;
}

std::vector<float> read_matte(const std::string& render_dir, const std::string& clip_stem,
                              std::int64_t frame, int width, int height) {
    std::vector<float> values;
    int w = 0, h = 0;
    const fs::path path = fs::path(render_dir) / (clip_stem + "_mattes") / frame_name(frame);
    if (!matting::read_png16(path.string(), &values, &w, &h) || w != width || h != height) return {};
    return values;
}

std::vector<float> read_shown_matte(const std::string& render_dir, const std::string& clip_stem,
                                    const fixes::Fixes& fixes, std::int64_t frame, int width,
                                    int height) {
    if (fixes.plate.set()) {
        std::vector<float> values;
        int w = 0, h = 0;
        const fs::path path = fs::path(render_dir) / fixes::plate_mattes_name(clip_stem, fixes.plate) /
                              frame_name(frame);
        if (matting::read_png16(path.string(), &values, &w, &h) && w == width && h == height)
            return values;
    }
    return read_matte(render_dir, clip_stem, frame, width, height);
}

bool obtain_plate(const std::string& render_dir, const std::string& clip_stem,
                  const fixes::PlateRange& range, gpu::GpuContext& gpu, gpu::IngestPass& ingest,
                  media::VideoDecoder& decoder, const gpu::InputTransform& transform,
                  matting::PlateImage* plate, std::string* error) {
    const fs::path path = fs::path(render_dir) / fixes::plate_image_name(clip_stem, range);
    if (matting::load_plate(path.string(), plate) && plate->width == decoder.width() &&
        plate->height == decoder.height())
        return true;

    matting::PlateBuilder builder;
    for (const std::int64_t f : fixes::plate_frames(range)) {
        if (f >= decoder.frame_count() || !decoder.seek_frame(f)) {
            *error = "cannot decode plate frame " + std::to_string(f);
            return false;
        }
        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu.device());
        const bool ingested = ingest.process(cmd, decoder, transform);
        SDL_SubmitGPUCommandBuffer(cmd);
        if (!ingested) {
            *error = "cannot read plate frame " + std::to_string(f);
            return false;
        }
        builder.add(matting::prepare_frame(ingest.read_working(), static_cast<int>(ingest.width()),
                                           static_cast<int>(ingest.height())));
    }
    *plate = builder.finish();
    if (plate->empty()) {
        *error = "no frames for the plate";
        return false;
    }
    // Written beside and renamed, like everything else a viewer reads.
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    fs::path partial = path;
    partial.replace_extension(".partial.png");
    if (!matting::save_plate(partial.string(), *plate)) {
        *error = "cannot write " + partial.string();
        return false;
    }
    fs::rename(partial, path, ec);
    if (ec) {
        *error = "cannot rename " + partial.string() + ": " + ec.message();
        return false;
    }
    // Plates made from other frames are cleared: seconds to make again,
    // and only this one is wanted. Only files named exactly as plates are.
    const std::string prefix = clip_stem + "_plate_";
    for (fs::directory_iterator it(path.parent_path(), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name == path.filename().string() || name.compare(0, prefix.size(), prefix) != 0) continue;
        long long a = 0, b = 0;
        char tail[8] = {};
        if (std::sscanf(name.c_str() + prefix.size(), "%lld_%lld%7s", &a, &b, tail) == 3 &&
            std::string(tail) == ".png" &&
            name == fixes::plate_image_name(clip_stem, fixes::PlateRange{a, b, false})) {
            std::error_code removed;
            fs::remove(it->path(), removed);
        }
    }
    std::printf("made the clean plate from %d frames: %s\n", builder.count(), path.c_str());
    std::fflush(stdout);
    return true;
}

std::vector<float> plate_pass(const matting::MattingFrame& frame, const std::vector<float>& alpha,
                              const matting::PlateImage& plate, bool correct, std::string* error,
                              std::vector<float>* wall_out, float* tolerance_out) {
    std::vector<float> wall = matting::relight_plate(plate, frame, alpha);
    if (wall.empty()) {
        *error = "the plate is not the frame's size";
        return {};
    }
    const float tolerance = matting::plate_tolerance(frame, wall, alpha);
    std::vector<float> refined =
        correct ? matting::correct_with_plate(frame, wall, alpha, tolerance) : alpha;
    // The corrected matte moved; the plate is relit from what it now says
    // is wall before the band leans on it.
    if (correct) wall = matting::relight_plate(plate, frame, refined);
    refined = matting::MattingStack::band(frame, refined, error, &wall, tolerance);
    if (wall_out) *wall_out = std::move(wall);
    if (tolerance_out) *tolerance_out = tolerance;
    return refined;
}

void MatteComposer::open(const std::string& render_dir, const std::string& clip_stem,
                         const fixes::Fixes& fixes, int width, int height, std::int64_t frame_count,
                         const std::string& models, bool model_cpu, const matting::PlateImage* plate) {
    render_dir_ = render_dir;
    stem_ = clip_stem;
    fixes_ = fixes;
    width_ = width;
    height_ = height;
    models_ = models;
    model_cpu_ = model_cpu;
    plate_ = plate;
    jobs_ = fixes::rematte_jobs(fixes, frame_count, width, height);
    stack_.reset();
}

bool MatteComposer::check(std::int64_t first, std::int64_t last, std::string* error) const {
    std::error_code ec;
    const fs::path mattes = fs::path(render_dir_) / (stem_ + "_mattes");
    const fs::path plate_mattes = fs::path(render_dir_) / fixes::plate_mattes_name(stem_, fixes_.plate);
    for (std::int64_t f = first; f <= last; ++f)
        if (!fs::exists(mattes / frame_name(f), ec) ||
            (fixes_.plate.set() && !fs::exists(plate_mattes / frame_name(f), ec))) {
            *error = "frame " + std::to_string(f) + " has no matte" +
                     (fixes_.plate.set() ? " refined by the plate" : "") + "; matte the clip first";
            return false;
        }
    return true;
}

std::vector<float> MatteComposer::compose(std::int64_t f, const matting::MattingFrame& frame,
                                          std::string* error) {
    std::vector<float> alpha = read_shown_matte(render_dir_, stem_, fixes_, f, width_, height_);
    if (alpha.empty()) {
        *error = "cannot read the matte";
        return {};
    }
    // The patches, in the order the viewer folds them, each blended over the
    // frame's own matte as it was before any of them.
    const fs::path patches_dir = fs::path(render_dir_) / (stem_ + "_rematte");
    const std::vector<float> base = alpha;
    std::error_code ec;
    for (const fixes::PatchJob& j : jobs_) {
        if (j.frame != f) continue;
        const std::string path = (patches_dir / fixes::patch_name(f, j.box)).string();
        std::vector<float> patch;
        int pw = 0, ph = 0;
        if (!matting::read_png16(path, &patch, &pw, &ph) || pw != j.box.width() || ph != j.box.height()) {
            if (!stack_) {
                stack_ = std::make_unique<matting::MattingStack>();
                const auto backend =
                    model_cpu_ ? matting::MatteModel::Backend::kCpu : matting::MatteModel::Backend::kGpu;
                if (!stack_->load(models_, backend, matting::default_cache_dir(), error)) {
                    *error = "cannot load the models in " + models_ + ": " + *error;
                    stack_.reset();
                    return {};
                }
            }
            std::vector<float> wall;
            float tolerance = 0.0f;
            if (plate_) {
                wall = matting::relight_plate(*plate_, frame, base);
                tolerance = matting::plate_tolerance(frame, wall, base);
            }
            patch = make_patch(*stack_, frame, base, j.box, nullptr, error, wall.empty() ? nullptr : &wall,
                               tolerance);
            fs::create_directories(patches_dir, ec);
            if (patch.empty() || !write_patch(path, patch, j.box, error)) return {};
        }
        fixes::blend_patch(alpha, width_, j.box, patch);
    }
    fixes::apply_fixes(fixes_, f, alpha, width_, height_);
    return alpha;
}

bool run_export(const ExportRequest& request, gpu::GpuContext& gpu, gpu::IngestPass& ingest,
                media::VideoDecoder& decoder, const gpu::InputTransform& transform) {
    const std::string stem = fs::path(request.clip).stem().string();
    const media::ExportFormatInfo& info = media::export_format_info(request.format);
    const int w = decoder.width(), h = decoder.height();
    const std::int64_t total = decoder.frame_count();
    const std::int64_t first = std::clamp<std::int64_t>(request.first, 0, std::max<std::int64_t>(total - 1, 0));
    const std::int64_t last =
        request.last < 0 ? total - 1 : std::clamp<std::int64_t>(request.last, first, total - 1);

    // Everything checked before anything is written: a matte for every frame.
    std::error_code ec;
    fixes::Fixes fixes;
    std::string error;
    if (!fixes.load((fs::path(request.render_dir) / (stem + "_fixes.json")).string(), &error)) {
        std::fprintf(stderr, "cannot read the fixes: %s\n", error.c_str());
        return false;
    }
    matting::PlateImage plate;
    if (fixes.plate.set() &&
        !obtain_plate(request.render_dir, stem, fixes.plate, gpu, ingest, decoder, transform, &plate,
                      &error)) {
        std::fprintf(stderr, "cannot make the clean plate: %s\n", error.c_str());
        return false;
    }
    MatteComposer composer;
    composer.open(request.render_dir, stem, fixes, w, h, total, request.models, request.model_cpu,
                  plate.empty() ? nullptr : &plate);
    if (!composer.check(first, last, &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }

    const fs::path out(request.out);
    const fs::path partial = partial_of(out);
    media::VideoEncoder encoder;
    const bool sequence = request.format == media::ExportFormat::kPngSequence;
    if (sequence) {
        // A stopped export's leftovers are cleared; anything else is not.
        if (fs::exists(partial, ec)) {
            if (!only_frames_of(partial, out.filename().string())) {
                std::fprintf(stderr, "%s is in the way\n", partial.c_str());
                return false;
            }
            fs::remove_all(partial, ec);
        }
        fs::create_directories(partial, ec);
        if (ec) {
            std::fprintf(stderr, "cannot make %s: %s\n", partial.c_str(), ec.message().c_str());
            return false;
        }
    } else {
        fs::create_directories(out.parent_path(), ec);
        if (!encoder.open(partial.string(), request.format, w, h, decoder.rate_num(),
                          decoder.rate_den(), &error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return false;
        }
    }
    std::printf("exporting %s, frames %lld-%lld, as %s, to %s\n", request.clip.c_str(),
                static_cast<long long>(first), static_cast<long long>(last), info.label,
                out.c_str());
    std::fflush(stdout);

    if (!decoder.seek_frame(first)) {
        std::fprintf(stderr, "cannot decode frame %lld\n", static_cast<long long>(first));
        return false;
    }
    const Uint64 started = SDL_GetTicksNS();
    const std::size_t pixels = static_cast<std::size_t>(w) * h;
    std::vector<std::uint16_t> samples(pixels * (info.subject() ? 4 : 1));
    for (std::int64_t f = first; f <= last; ++f) {
        if (f > first && !decoder.read_next()) {
            std::fprintf(stderr, "cannot decode frame %lld\n", static_cast<long long>(f));
            return false;
        }
        const Uint64 t = SDL_GetTicksNS();
        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu.device());
        const bool ingested = ingest.process(cmd, decoder, transform);
        SDL_SubmitGPUCommandBuffer(cmd);
        if (!ingested) return false;
        const matting::MattingFrame frame = matting::prepare_frame(
            ingest.read_working(), static_cast<int>(ingest.width()), static_cast<int>(ingest.height()));

        const std::vector<float> alpha = composer.compose(f, frame, &error);
        if (alpha.empty()) {
            std::fprintf(stderr, "frame %lld: %s\n", static_cast<long long>(f), error.c_str());
            return false;
        }

        if (info.subject()) {
            // The subject's own colour, the wall's spill taken out of its
            // edges -- straight, not premultiplied, as all three formats
            // carry it.
            std::vector<float> image(pixels * 3);
            for (std::size_t i = 0; i < pixels; ++i)
                for (int c = 0; c < 3; ++c)
                    image[i * 3 + c] = std::clamp(frame.linear[c * pixels + i], 0.0f, 1.0f);
            // With a plate, the wall behind is known, and the subject's
            // colour follows from it exactly where there is enough of it.
            std::vector<float> fg;
            if (!plate.empty()) {
                const std::vector<float> wall = matting::relight_plate(plate, frame, alpha);
                // (relit from the final matte, so the wall is measured where
                // the export says it shows)
                std::vector<float> wall_rgb(pixels * 3);
                for (std::size_t i = 0; i < pixels; ++i)
                    for (int c = 0; c < 3; ++c)
                        wall_rgb[i * 3 + c] = std::clamp(wall[c * pixels + i], 0.0f, 1.0f);
                fg = matting::estimate_foreground_known(image, alpha, wall_rgb, w, h);
            } else {
                fg = matting::estimate_foreground_ml(image, alpha, w, h);
            }
            for (std::size_t i = 0; i < pixels; ++i) {
                for (int c = 0; c < 3; ++c) samples[i * 4 + c] = matting::to_sample(encode_709(fg[i * 3 + c]));
                samples[i * 4 + 3] = matting::to_sample(alpha[i]);
            }
        } else {
            for (std::size_t i = 0; i < pixels; ++i) samples[i] = matting::to_sample(alpha[i]);
        }

        if (sequence) {
            char name[64];
            std::snprintf(name, sizeof name, "%s_%06lld.png", out.filename().c_str(),
                          static_cast<long long>(f));
            if (!matting::write_png16_rgba((partial / name).string(), samples, w, h)) {
                std::fprintf(stderr, "cannot write %s\n", (partial / name).c_str());
                return false;
            }
        } else if (!encoder.write(samples.data(), &error)) {
            std::fprintf(stderr, "frame %lld: %s\n", static_cast<long long>(f), error.c_str());
            return false;
        }

        const std::int64_t done = f - first + 1, count = last - first + 1;
        const double per = (SDL_GetTicksNS() - started) / 1e9 / static_cast<double>(done);
        std::printf("  %lld/%lld  frame %lld  %.2f s  eta %.1f min\n", static_cast<long long>(done),
                    static_cast<long long>(count), static_cast<long long>(f),
                    (SDL_GetTicksNS() - t) / 1e9, per * static_cast<double>(count - done) / 60.0);
        std::fflush(stdout);
    }

    if (!sequence && !encoder.finish(&error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    // Whole: into place. Over a file of the same name, which the save dialog
    // has asked about; over a folder only if it holds nothing but an earlier
    // export's frames -- never anything else that happens to have the name.
    if (fs::is_directory(out, ec)) {
        if (!only_frames_of(out, out.filename().string())) {
            std::fprintf(stderr, "%s is a folder with other things in it; the export is at %s\n",
                         out.c_str(), partial.c_str());
            return false;
        }
        fs::remove_all(out, ec);
    } else if (fs::exists(out, ec)) {
        fs::remove(out, ec);
    }
    fs::rename(partial, out, ec);
    if (ec) {
        std::fprintf(stderr, "cannot rename %s: %s\n", partial.c_str(), ec.message().c_str());
        return false;
    }
    std::printf("exported %s\n", out.c_str());
    return true;
}

}  // namespace ps::exporting

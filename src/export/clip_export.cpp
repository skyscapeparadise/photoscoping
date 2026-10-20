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
                              float* detail, std::string* error) {
    const fixes::RematteCrop c = fixes::rematte_crop(box, frame.width, frame.height);
    if (detail) *detail = c.detail;
    const std::vector<float> alpha = stack.closer(frame, base, c.crop.x0, c.crop.y0, c.crop.width(),
                                                  c.crop.height(), c.wide, error);
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
    const fs::path mattes = fs::path(request.render_dir) / (stem + "_mattes");
    for (std::int64_t f = first; f <= last; ++f)
        if (!fs::exists(mattes / frame_name(f), ec)) {
            std::fprintf(stderr, "frame %lld has no matte; matte the clip first\n",
                         static_cast<long long>(f));
            return false;
        }

    fixes::Fixes fixes;
    std::string error;
    if (!fixes.load((fs::path(request.render_dir) / (stem + "_fixes.json")).string(), &error)) {
        std::fprintf(stderr, "cannot read the fixes: %s\n", error.c_str());
        return false;
    }
    const fs::path patches_dir = fs::path(request.render_dir) / (stem + "_rematte");
    std::vector<fixes::PatchJob> jobs = fixes::rematte_jobs(fixes, total, w, h);

    // The models, only if a patch is missing.
    std::unique_ptr<matting::MattingStack> stack;
    auto need_stack = [&]() -> matting::MattingStack* {
        if (stack) return stack.get();
        stack = std::make_unique<matting::MattingStack>();
        const auto backend = request.model_cpu ? matting::MatteModel::Backend::kCpu
                                               : matting::MatteModel::Backend::kGpu;
        if (!stack->load(request.models, backend, matting::default_cache_dir(), &error)) {
            std::fprintf(stderr, "cannot load the models in %s: %s\n", request.models.c_str(),
                         error.c_str());
            stack.reset();
        }
        return stack.get();
    };

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
    std::vector<std::uint16_t> samples(pixels * (info.subject ? 4 : 1));
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

        std::vector<float> alpha = read_matte(request.render_dir, stem, f, w, h);
        if (alpha.empty()) {
            std::fprintf(stderr, "cannot read the matte for frame %lld\n", static_cast<long long>(f));
            return false;
        }
        // The patches, in the order the viewer folds them, each blended over
        // the frame's own matte as it was before any of them.
        const std::vector<float> base = alpha;
        for (const fixes::PatchJob& j : jobs) {
            if (j.frame != f) continue;
            const std::string path = (patches_dir / fixes::patch_name(f, j.box)).string();
            std::vector<float> patch;
            int pw = 0, ph = 0;
            if (!matting::read_png16(path, &patch, &pw, &ph) || pw != j.box.width() ||
                ph != j.box.height()) {
                matting::MattingStack* s = need_stack();
                if (!s) return false;
                patch = make_patch(*s, frame, base, j.box, nullptr, &error);
                fs::create_directories(patches_dir, ec);
                if (patch.empty() || !write_patch(path, patch, j.box, &error)) {
                    std::fprintf(stderr, "frame %lld: %s\n", static_cast<long long>(f), error.c_str());
                    return false;
                }
            }
            fixes::blend_patch(alpha, w, j.box, patch);
        }
        fixes::apply_fixes(fixes, f, alpha, w, h);

        if (info.subject) {
            // The subject's own colour, the wall's spill taken out of its
            // edges -- straight, not premultiplied, as all three formats
            // carry it.
            std::vector<float> image(pixels * 3);
            for (std::size_t i = 0; i < pixels; ++i)
                for (int c = 0; c < 3; ++c)
                    image[i * 3 + c] = std::clamp(frame.linear[c * pixels + i], 0.0f, 1.0f);
            const std::vector<float> fg = matting::estimate_foreground_ml(image, alpha, w, h);
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

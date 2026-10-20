#include "app/rematte_patches.hpp"

#include <SDL3_image/SDL_image.h>

#include <algorithm>
#include <filesystem>

namespace ps::app {

namespace {

namespace fs = std::filesystem;

// A finished patch: frame and box, and .png. The .partial.png one is written
// under is not.
bool is_patch(const std::string& name) {
    return name.size() > 4 && name.compare(name.size() - 4, 4, ".png") == 0 &&
           name.find(".partial") == std::string::npos;
}

}  // namespace

RemattePatches::~RemattePatches() { destroy(); }

void RemattePatches::open(SDL_GPUDevice* device, const std::string& dir, int frame_w, int frame_h,
                          std::int64_t frame_count) {
    destroy();
    texture_.create(device);
    dir_ = dir;
    frame_w_ = frame_w;
    frame_h_ = frame_h;
    frame_count_ = frame_count;
    rescan();

    start([this](std::int64_t frame) {
        std::vector<fixes::PixelRect> boxes;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            loaded_generation_ = generation_;
            const auto it = by_frame_.find(frame);
            if (it != by_frame_.end()) boxes = it->second;
        }
        loaded_.clear();
        if (boxes.empty()) return true;

        fixes::PixelRect u = boxes.front();
        for (const fixes::PixelRect& b : boxes) {
            u.x0 = std::min(u.x0, b.x0);
            u.y0 = std::min(u.y0, b.y0);
            u.x1 = std::max(u.x1, b.x1);
            u.y1 = std::max(u.y1, b.y1);
        }
        // Folded in turn, each over what is there: alpha kept premultiplied
        // by its weight while they stack, and divided out at the end.
        const int uw = u.width(), uh = u.height();
        std::vector<float> taken(static_cast<std::size_t>(uw) * uh, 0.0f);
        std::vector<float> weight(taken.size(), 0.0f);
        for (const fixes::PixelRect& b : boxes) {
            SDL_Surface* file = IMG_Load((fs::path(dir_) / fixes::patch_name(frame, b)).string().c_str());
            if (!file) continue;
            SDL_Surface* rgba = SDL_ConvertSurface(file, SDL_PIXELFORMAT_ABGR8888);
            SDL_DestroySurface(file);
            if (!rgba) continue;
            if (rgba->w == b.width() && rgba->h == b.height()) {
                for (int y = 0; y < b.height(); ++y) {
                    const auto* row = static_cast<const std::uint8_t*>(rgba->pixels) + y * rgba->pitch;
                    for (int x = 0; x < b.width(); ++x) {
                        const float w = fixes::rematte_weight(b, b.x0 + x, b.y0 + y);
                        const std::size_t i =
                            static_cast<std::size_t>(b.y0 - u.y0 + y) * uw + (b.x0 - u.x0 + x);
                        taken[i] = taken[i] * (1.0f - w) + row[x * 4] / 255.0f * w;
                        weight[i] = weight[i] * (1.0f - w) + w;
                    }
                }
            }
            SDL_DestroySurface(rgba);
        }
        loaded_.assign(taken.size() * 4, 0);
        for (std::size_t i = 0; i < taken.size(); ++i) {
            const float a = weight[i] > 0.0f ? taken[i] / weight[i] : 0.0f;
            loaded_[i * 4 + 0] = static_cast<std::uint8_t>(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
            loaded_[i * 4 + 1] = static_cast<std::uint8_t>(std::clamp(weight[i], 0.0f, 1.0f) * 255.0f + 0.5f);
            loaded_[i * 4 + 3] = 255;
        }
        loaded_rect_ = u;
        return true;
    });
}

void RemattePatches::destroy() {
    stop();
    texture_.destroy();
    loaded_.clear();
    jobs_.clear();
    files_.clear();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        by_frame_.clear();
    }
    made_count_ = frames_to_make_ = 0;
    has_ = false;
}

void RemattePatches::set_fixes(const fixes::Fixes& fixes) {
    jobs_ = fixes::rematte_jobs(fixes, frame_count_, frame_w_, frame_h_);
    relink();
}

bool RemattePatches::rescan() {
    std::set<std::string> files;
    std::error_code ec;
    for (fs::directory_iterator it(dir_, ec), end; !ec && it != end; it.increment(ec)) {
        std::string name = it->path().filename().string();
        if (is_patch(name)) files.insert(std::move(name));
    }
    if (files == files_) return false;
    files_ = std::move(files);
    relink();
    return true;
}

void RemattePatches::relink() {
    std::unordered_map<std::int64_t, std::vector<fixes::PixelRect>> by_frame;
    std::size_t made = 0;
    std::set<std::int64_t> unmade;
    for (const fixes::PatchJob& j : jobs_) {
        if (files_.count(fixes::patch_name(j.frame, j.box))) {
            by_frame[j.frame].push_back(j.box);
            ++made;
        } else {
            unmade.insert(j.frame);
        }
    }
    made_count_ = made;
    frames_to_make_ = unmade.size();
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        changed = by_frame != by_frame_;
        by_frame_ = std::move(by_frame);
        if (changed) ++generation_;
    }
    if (changed) refresh();
}

bool RemattePatches::wants(std::int64_t frame) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return by_frame_.count(frame) != 0;
}

bool RemattePatches::made(std::int64_t frame, const fixes::PixelRect& box) const {
    return files_.count(fixes::patch_name(frame, box)) != 0;
}

void RemattePatches::rect_uv(float out[4]) const {
    const float w = static_cast<float>(std::max(frame_w_, 1));
    const float h = static_cast<float>(std::max(frame_h_, 1));
    out[0] = rect_.x0 / w;
    out[1] = rect_.y0 / h;
    out[2] = rect_.x1 / w;
    out[3] = rect_.y1 / h;
}

bool RemattePatches::upload(SDL_GPUCommandBuffer* cmd) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (loaded_generation_ != generation_) return false;  // made from old patches
    }
    has_ = !loaded_.empty() &&
           texture_.upload(cmd, loaded_, loaded_rect_.width(), loaded_rect_.height());
    if (has_) rect_ = loaded_rect_;
    return true;
}

}  // namespace ps::app

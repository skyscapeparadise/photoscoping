#include "app/light_preview.hpp"

#include <algorithm>
#include <cmath>

#include "color/color_space.hpp"
#include "fixes/mask.hpp"
#include "media/exr_writer.hpp"
#include "media/half.hpp"

namespace ps::app {

namespace {

constexpr int kPreviewSize = 512;  // the long side

}  // namespace

LightPreview::~LightPreview() { destroy(); }

void LightPreview::create(SDL_GPUDevice* device, std::function<void()> notify) {
    destroy();
    texture_.create(device);
    notify_ = std::move(notify);
    stopping_ = false;
    thread_ = std::thread([this] { loop(); });
}

void LightPreview::destroy() {
    if (thread_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_all();
        thread_.join();
    }
    texture_.destroy();
    pending_frame_.reset();
    pending_crop_.reset();
    ready_ = false;
    shown_ = false;
    frame_ = -1;
}

bool LightPreview::has_frame(std::int64_t frame, std::uint64_t version, bool fill_shadows) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_frame_)
        return pending_frame_->frame == frame && pending_frame_->version == version &&
               pending_frame_->fill_shadows == fill_shadows;
    return frame_ == frame && version_ == version && fill_shadows_ == fill_shadows;
}

void LightPreview::request(std::optional<Frame> frame, Crop crop) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (frame) pending_frame_ = std::move(frame);
        pending_crop_ = std::move(crop);
    }
    wake_.notify_all();
}

bool LightPreview::take(SDL_GPUCommandBuffer* cmd) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready_) return false;
    ready_ = false;
    if (!texture_.upload(cmd, made_, made_w_, made_h_)) return false;
    shown_ = true;
    shown_w_ = made_w_;
    shown_h_ = made_h_;
    return true;
}

void LightPreview::wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    wake_.wait(lock, [&] { return stopping_ || (!pending_frame_ && !pending_crop_ && !busy_); });
}

void LightPreview::loop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        wake_.wait(lock, [&] { return stopping_ || pending_frame_ || pending_crop_; });
        if (stopping_) return;
        std::optional<Frame> frame = std::move(pending_frame_);
        std::optional<Crop> crop = std::move(pending_crop_);
        pending_frame_.reset();
        pending_crop_.reset();
        busy_ = true;
        lock.unlock();
        if (frame) prepare(std::move(*frame));
        std::vector<std::uint8_t> rgba;
        int w = 0, h = 0;
        const bool made = crop && render(*crop, &rgba, &w, &h);
        lock.lock();
        busy_ = false;
        // Kept only if nothing newer has been asked for meanwhile.
        if (made && !pending_crop_) {
            made_ = std::move(rgba);
            made_w_ = w;
            made_h_ = h;
            ready_ = true;
        }
        wake_.notify_all();
        if (ready_ && notify_) {
            lock.unlock();
            notify_();
            lock.lock();
        }
    }
}

void LightPreview::prepare(Frame in) {
    const int w = in.width, h = in.height;
    const std::size_t n = static_cast<std::size_t>(w) * h;
    if (w <= 0 || in.working.size() != n * 4 || in.coverage.size() != n) return;

    // The light, linear Rec.709, as the export's frames hold it.
    const color::Mat3 to709 = color::working_to_rec709();
    linear_.assign(n * 3, 0.0f);
    for (std::size_t i = 0; i < n; ++i) {
        const float r = media::half_to_float(in.working[i * 4]), g = media::half_to_float(in.working[i * 4 + 1]),
                    b = media::half_to_float(in.working[i * 4 + 2]);
        for (int c = 0; c < 3; ++c)
            linear_[c * n + i] = to709[c * 3] * r + to709[c * 3 + 1] * g + to709[c * 3 + 2] * b;
    }
    // The matte as the viewer shows it: the stack's, the patches, the fixes.
    alpha_.assign(n, 0.0f);
    for (std::size_t i = 0; i < n; ++i) alpha_[i] = in.coverage[i] / 255.0f;
    const fixes::PixelRect& r = in.patch_rect;
    if (!in.patches.empty() && in.patches.size() == static_cast<std::size_t>(r.width()) * r.height() * 4) {
        for (int y = r.y0; y < r.y1; ++y)
            for (int x = r.x0; x < r.x1; ++x) {
                const std::size_t p = (static_cast<std::size_t>(y - r.y0) * r.width() + (x - r.x0)) * 4;
                const float weight = in.patches[p + 1] / 255.0f;
                float& a = alpha_[static_cast<std::size_t>(y) * w + x];
                a = a * (1.0f - weight) + in.patches[p] / 255.0f * weight;
            }
    }
    fixes::apply_fixes(in.fixes, in.frame, alpha_, w, h);
    // The plate, read once per plate.
    if (in.plate_path != plate_path_) {
        plate_path_ = in.plate_path;
        plate_ = {};
        // The scene-light plate is float; the other, a PNG.
        if (plate_path_.size() > 4 && plate_path_.compare(plate_path_.size() - 4, 4, ".exr") == 0)
            media::read_exr_half(plate_path_, &plate_.linear, &plate_.width, &plate_.height);
        else if (!plate_path_.empty())
            matting::load_plate(plate_path_, &plate_);
    }
    const bool plate_fits = !plate_.empty() && plate_.width == w && plate_.height == h;
    light_ = light::prepare_frame_light(linear_, alpha_, w, h, plate_fits ? &plate_.linear : nullptr,
                                        in.fill_shadows);
    std::lock_guard<std::mutex> lock(mutex_);
    frame_ = in.frame;
    version_ = in.version;
    fill_shadows_ = in.fill_shadows;
    width_ = w;
    height_ = h;
}

bool LightPreview::render(const Crop& c, std::vector<std::uint8_t>* rgba, int* width, int* height) {
    if (linear_.empty() || c.crop.point_count() != 4) return false;
    const std::vector<fixes::Point> corners = c.crop.keys.front().points;
    int w = 0, h = 0;
    light::output_size(corners, kPreviewSize, &w, &h);
    const std::vector<float> filled = light::render_crop(light_, corners, w, h);
    const std::size_t n = static_cast<std::size_t>(w) * h;
    // This frame's exposure: its brightest light just under white.
    float scale = 1.0f;
    if (c.linear) {
        float peak = 0.0f;
        for (std::size_t i = 0; i + 1 < n; ++i)
            for (int k = 0; k < 3; ++k) peak = std::max(peak, 0.5f * (filled[k * n + i] + filled[k * n + i + 1]));
        if (peak > 1e-4f) scale = 0.95f / peak;
    }
    const std::vector<float> encoded = light::encode_crop(filled, c.linear, scale, c.crop, w, h);
    rgba->assign(n * 4, 255);
    for (std::size_t i = 0; i < n; ++i)
        for (int k = 0; k < 3; ++k)
            (*rgba)[i * 4 + k] =
                static_cast<std::uint8_t>(std::lround(std::clamp(encoded[k * n + i], 0.0f, 1.0f) * 255.0f));
    *width = w;
    *height = h;
    return true;
}

}  // namespace ps::app

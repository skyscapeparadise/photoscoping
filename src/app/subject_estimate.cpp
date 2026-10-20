#include "app/subject_estimate.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "color/color_space.hpp"
#include "fixes/mask.hpp"
#include "matting/foreground.hpp"

namespace ps::app {

namespace {

float half_to_float(std::uint16_t h) {
    const std::uint32_t sign = (h & 0x8000u) << 16;
    const std::uint32_t exponent = (h >> 10) & 0x1Fu;
    const std::uint32_t mantissa = h & 0x3FFu;
    std::uint32_t bits;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            // Subnormal: normalised into a float.
            int e = -1;
            std::uint32_t m = mantissa;
            do {
                ++e;
                m <<= 1;
            } while ((m & 0x400u) == 0);
            bits = sign | static_cast<std::uint32_t>(127 - 15 - e) << 23 | (m & 0x3FFu) << 13;
        }
    } else if (exponent == 31) {
        bits = sign | 0x7F800000u | mantissa << 13;
    } else {
        bits = sign | (exponent + 127 - 15) << 23 | mantissa << 13;
    }
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

// Rounded to nearest; what the estimate makes is 0..1, so the normal range
// and zero are all that need to be right.
std::uint16_t float_to_half(float f) {
    if (!(f > 6.1e-5f)) return 0;  // zero, negatives and subnormals
    if (f >= 65504.0f) return 0x7BFF;
    std::uint32_t bits;
    std::memcpy(&bits, &f, sizeof bits);
    const std::uint32_t exponent = ((bits >> 23) & 0xFFu) - 127 + 15;
    std::uint32_t mantissa = bits & 0x7FFFFFu;
    std::uint32_t half = exponent << 10 | mantissa >> 13;
    if ((mantissa & 0x1FFFu) > 0x1000u || ((mantissa & 0x1FFFu) == 0x1000u && (half & 1u))) ++half;
    return static_cast<std::uint16_t>(half);
}

}  // namespace

SubjectEstimate::~SubjectEstimate() { destroy(); }

void SubjectEstimate::create(SDL_GPUDevice* device, std::function<void()> notify) {
    destroy();
    texture_.create(device, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, 8);
    notify_ = std::move(notify);
    stopping_ = false;
    thread_ = std::thread([this] { loop(); });
}

void SubjectEstimate::destroy() {
    if (thread_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_all();
        thread_.join();
    }
    texture_.destroy();
    pending_.reset();
    ready_ = false;
    made_.clear();
    asked_frame_ = shown_frame_ = -1;
}

void SubjectEstimate::request(Inputs inputs) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        asked_frame_ = inputs.frame;
        asked_version_ = inputs.version;
        pending_ = std::move(inputs);
    }
    wake_.notify_all();
}

bool SubjectEstimate::asked(std::int64_t frame, std::uint64_t version) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return asked_frame_ == frame && asked_version_ == version;
}

bool SubjectEstimate::take(SDL_GPUCommandBuffer* cmd) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready_) return false;
    ready_ = false;
    if (!texture_.upload(cmd, made_.data(), made_.size() * sizeof(std::uint16_t), made_w_, made_h_))
        return false;
    shown_frame_ = made_frame_;
    shown_version_ = made_version_;
    return true;
}

void SubjectEstimate::wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    wake_.wait(lock, [&] { return stopping_ || (!pending_ && !busy_); });
}

void SubjectEstimate::loop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        wake_.wait(lock, [&] { return stopping_ || pending_.has_value(); });
        if (stopping_) return;
        Inputs in = std::move(*pending_);
        pending_.reset();
        busy_ = true;
        lock.unlock();
        int w = 0, h = 0;
        std::vector<std::uint16_t> made = estimate(in, &w, &h);
        lock.lock();
        busy_ = false;
        // Kept only if nothing newer has been asked for meanwhile.
        if (!made.empty() && !pending_) {
            made_ = std::move(made);
            made_w_ = w;
            made_h_ = h;
            made_frame_ = in.frame;
            made_version_ = in.version;
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

std::vector<std::uint16_t> SubjectEstimate::estimate(const Inputs& in, int* width, int* height) {
    const int w = in.width, h = in.height;
    const std::size_t pixels = static_cast<std::size_t>(w) * h;
    if (w < 2 || h < 2 || in.working.size() != pixels * 4 || in.coverage.size() != pixels) return {};

    // The matte as the viewer shows it, at full size: the stack's, the
    // patches over it, then the fixes.
    std::vector<float> alpha(pixels);
    for (std::size_t i = 0; i < pixels; ++i) alpha[i] = in.coverage[i] / 255.0f;
    const fixes::PixelRect& r = in.patch_rect;
    if (!in.patches.empty() &&
        in.patches.size() == static_cast<std::size_t>(r.width()) * r.height() * 4) {
        for (int y = r.y0; y < r.y1; ++y)
            for (int x = r.x0; x < r.x1; ++x) {
                const std::size_t p = (static_cast<std::size_t>(y - r.y0) * r.width() + (x - r.x0)) * 4;
                const float weight = in.patches[p + 1] / 255.0f;
                float& a = alpha[static_cast<std::size_t>(y) * w + x];
                a = a * (1.0f - weight) + in.patches[p] / 255.0f * weight;
            }
    }
    fixes::apply_fixes(in.fixes, in.frame, alpha, w, h);

    // Half size, each pixel the mean of four; the footage to linear Rec.709,
    // held to 0..1, as the export estimates it.
    const int hw = w / 2, hh = h / 2;
    const std::size_t half = static_cast<std::size_t>(hw) * hh;
    const color::Mat3 to709 = color::working_to_rec709();
    std::vector<float> image(half * 3), small_alpha(half);
    for (int y = 0; y < hh; ++y)
        for (int x = 0; x < hw; ++x) {
            float rgb[3] = {0.0f, 0.0f, 0.0f}, a = 0.0f;
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    const std::size_t s = static_cast<std::size_t>(y * 2 + dy) * w + (x * 2 + dx);
                    for (int c = 0; c < 3; ++c) rgb[c] += half_to_float(in.working[s * 4 + c]);
                    a += alpha[s];
                }
            const std::size_t d = static_cast<std::size_t>(y) * hw + x;
            for (int c = 0; c < 3; ++c) {
                const float v = (to709[c * 3] * rgb[0] + to709[c * 3 + 1] * rgb[1] + to709[c * 3 + 2] * rgb[2]) * 0.25f;
                image[d * 3 + c] = std::clamp(v, 0.0f, 1.0f);
            }
            small_alpha[d] = a * 0.25f;
        }
    const std::vector<float> fg = matting::estimate_foreground_ml(image, small_alpha, hw, hh);

    // Back to the working space, which the view converts for the display.
    const color::Mat3 to2020 = color::to_working(color::Gamut::kRec709);
    std::vector<std::uint16_t> out(half * 4);
    for (std::size_t i = 0; i < half; ++i) {
        for (int c = 0; c < 3; ++c)
            out[i * 4 + c] = float_to_half(to2020[c * 3] * fg[i * 3] + to2020[c * 3 + 1] * fg[i * 3 + 1] +
                                           to2020[c * 3 + 2] * fg[i * 3 + 2]);
        out[i * 4 + 3] = float_to_half(1.0f);
    }
    *width = hw;
    *height = hh;
    return out;
}

}  // namespace ps::app

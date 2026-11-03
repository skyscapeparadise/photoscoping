#include "gpu/compute_ops.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "gpu/shaders_generated.hpp"
#include "matting/image_ops.hpp"

namespace ps::gpu {

namespace {

// Uniform blocks, laid out as the shaders declare them.
struct Size2 {
    Uint32 width, height;
};
struct DilateParams {
    Uint32 width, height;
    float threshold;
};
struct PoolParams {
    Uint32 width, height, qw, qh, factor;
};
struct RelightParams {
    Uint32 width, height, qw, qh;
};
struct ResizeParams {
    Uint32 dw, dh, sw, sh, depth;
};
struct UpdateParams {
    Uint32 width, height, parity;
};
struct BlurParams {
    Uint32 width, height, depth;
    Sint32 radius;
    Uint32 vertical, pad0, pad1, pad2;
    float weights[68];
};
struct PackParams {
    Uint32 width, height, known;
};

constexpr int kMargin = 6;  // relight_plate's default: an edge's spill is not the wall's light

}  // namespace

SubjectGpu::SubjectGpu(SDL_GPUDevice* device) : device_(device), transfer_(device) {
    SDL_GPUSamplerCreateInfo info = {};
    info.min_filter = SDL_GPU_FILTER_NEAREST;
    info.mag_filter = SDL_GPU_FILTER_NEAREST;
    info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = SDL_CreateGPUSampler(device, &info);

    auto make = [&](const char* msl, Uint32 samplers, Uint32 ro, Uint32 rw, Uint32 gx = 16, Uint32 gy = 16) {
        KernelLayout l;
        l.samplers = samplers;
        l.readonly_buffers = ro;
        l.readwrite_buffers = rw;
        l.group_x = gx;
        l.group_y = gy;
        return create_kernel(device, msl, l);
    };
    prepare_ = make(shaders::kSubjectPrepare, 1, 0, 1);
    dilate_ = make(shaders::kDilateCross, 0, 1, 1);
    pool_ = make(shaders::kPoolRatio, 0, 3, 2);
    relight_ = make(shaders::kRelightApply, 0, 2, 1);
    row_sums_ = make(shaders::kRowSums, 0, 2, 1, 64, 1);
    resize_ = make(shaders::kResizeNearest, 0, 1, 1);
    update_ = make(shaders::kForegroundUpdate, 0, 2, 2);
    direct_ = make(shaders::kForegroundDirect, 0, 3, 1);
    blur_ = make(shaders::kBlurPass, 0, 1, 1);
    pack_ = make(shaders::kSubjectPack, 0, 4, 1);
    ok_ = sampler_ && prepare_ && dilate_ && pool_ && relight_ && row_sums_ && resize_ && update_ &&
          direct_ && blur_ && pack_;
}

SubjectGpu::~SubjectGpu() {
    for (Kernel* k : {&prepare_, &dilate_, &pool_, &relight_, &row_sums_, &resize_, &update_, &direct_,
                      &blur_, &pack_})
        release_kernel(device_, *k);
    if (sampler_) SDL_ReleaseGPUSampler(device_, sampler_);
}

bool SubjectGpu::set_plate(const std::vector<float>& linear, int width, int height) {
    has_plate_ = false;
    if (linear.empty()) return true;
    const std::size_t bytes = static_cast<std::size_t>(width) * height * 3 * sizeof(float);
    if (linear.size() * sizeof(float) != bytes || !plate_.reserve(device_, bytes) ||
        !transfer_.upload(plate_, linear))
        return false;
    has_plate_ = true;
    return true;
}

bool SubjectGpu::finish(SDL_GPUTexture* working, const std::vector<float>& alpha, int width, int height,
                        std::vector<std::uint16_t>* rgba) {
    if (!ok_) return false;
    const std::size_t n = static_cast<std::size_t>(width) * height;
    if (!lin_.reserve(device_, n * 3 * sizeof(float))) return false;
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (!cmd) return false;
    const Size2 p{static_cast<Uint32>(width), static_cast<Uint32>(height)};
    dispatch(cmd, {&prepare_, {}, {&lin_}, &p, sizeof p, working, sampler_, p.width, p.height});
    if (!SDL_SubmitGPUCommandBuffer(cmd)) return false;
    return run(alpha, width, height, rgba);
}

bool SubjectGpu::finish_linear(const std::vector<float>& linear, const std::vector<float>& alpha, int width,
                               int height, std::vector<std::uint16_t>* rgba) {
    if (!ok_) return false;
    const std::size_t n = static_cast<std::size_t>(width) * height;
    if (linear.size() != n * 3 || !lin_.reserve(device_, n * 3 * sizeof(float)) ||
        !transfer_.upload(lin_, linear))
        return false;
    return run(alpha, width, height, rgba);
}

bool SubjectGpu::run(const std::vector<float>& alpha, int width, int height, std::vector<std::uint16_t>* rgba) {
    const std::size_t n = static_cast<std::size_t>(width) * height;
    if (alpha.size() != n || !alpha_.reserve(device_, n * sizeof(float)) || !transfer_.upload(alpha_, alpha))
        return false;

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (!cmd) return false;
    const bool known = has_plate_ && plate_.size() >= n * 3 * sizeof(float);
    if (known && !relight(cmd, width, height)) return false;
    if (!foreground(cmd, width, height)) return false;
    if (known) {
        if (!direct_planes_.reserve(device_, n * 3 * sizeof(float)) ||
            !scratch_.reserve(device_, n * 3 * sizeof(float)))
            return false;
        const Size2 p{static_cast<Uint32>(width), static_cast<Uint32>(height)};
        dispatch(cmd, {&direct_, {&lin_, &alpha_, &wall_}, {&direct_planes_}, &p, sizeof p, nullptr, nullptr,
                       p.width, p.height});
        blur(cmd, direct_planes_, scratch_, width, height, 3, 1.5f);
    }
    if (!samples_.reserve(device_, n * 4 * sizeof(std::uint16_t))) return false;
    const PackParams pp{static_cast<Uint32>(width), static_cast<Uint32>(height), known ? 1u : 0u};
    // The final foreground is in f_[0] or f_[1]: foreground() leaves it in [0].
    const Buffer& direct = known ? direct_planes_ : f_[0];
    dispatch(cmd, {&pack_, {&lin_, &alpha_, &f_[0], &direct}, {&samples_}, &pp, sizeof pp, nullptr, nullptr,
                   pp.width, pp.height});
    if (!SDL_SubmitGPUCommandBuffer(cmd)) return false;
    rgba->resize(n * 4);
    return transfer_.download(samples_, *rgba);
}

bool SubjectGpu::relight(SDL_GPUCommandBuffer*& cmd, int width, int height) {
    const std::size_t n = static_cast<std::size_t>(width) * height;
    const Uint32 w = static_cast<Uint32>(width), h = static_cast<Uint32>(height);
    if (!near_a_.reserve(device_, n * sizeof(float)) || !near_b_.reserve(device_, n * sizeof(float)) ||
        !wall_.reserve(device_, n * 3 * sizeof(float)))
        return false;

    // Near the subject: the matte at 0.02 or more, widened by a cross kMargin
    // times.
    Buffer* from = &near_a_;
    Buffer* to = &near_b_;
    const DilateParams first{w, h, 0.02f};
    dispatch(cmd, {&dilate_, {&alpha_}, {from}, &first, sizeof first, nullptr, nullptr, w, h});
    const DilateParams step{w, h, 0.0f};
    for (int i = 1; i < kMargin; ++i) {
        dispatch(cmd, {&dilate_, {from}, {to}, &step, sizeof step, nullptr, nullptr, w, h});
        std::swap(from, to);
    }

    // The light as frame over plate, at a quarter of the size.
    const int qw = std::max(1, width / 4), qh = std::max(1, height / 4);
    const std::size_t qn = static_cast<std::size_t>(qw) * qh;
    if (!ratio_q_.reserve(device_, qn * 3 * sizeof(float)) || !weight_q_.reserve(device_, qn * sizeof(float)))
        return false;
    const PoolParams pp{w, h, static_cast<Uint32>(qw), static_cast<Uint32>(qh), 4};
    dispatch(cmd, {&pool_, {&lin_, &plate_, from}, {&ratio_q_, &weight_q_}, &pp, sizeof pp, nullptr, nullptr,
                   pp.qw, pp.qh});
    if (!submit_and_wait(device_, cmd)) return false;
    cmd = nullptr;

    // Filled and softened at quarter size, which is small: on the CPU.
    std::vector<float> ratio(qn * 3), weight(qn);
    if (!transfer_.download(ratio_q_, ratio) || !transfer_.download(weight_q_, weight)) return false;
    double visible = 0.0;
    for (float k : weight) visible += k;
    if (visible > static_cast<double>(n) / 100.0) {
        std::vector<std::uint8_t> measured(qn);
        for (std::size_t i = 0; i < qn; ++i) measured[i] = weight[i] >= 8.0f;  // half the block or more
        ratio = matting::fill_plate(ratio, qw, qh, measured);
        for (int c = 0; c < 3; ++c) {
            std::vector<float> plane(ratio.begin() + c * qn, ratio.begin() + (c + 1) * qn);
            plane = matting::gaussian_blur(plane, qw, qh, 2.0f);
            std::copy(plane.begin(), plane.end(), ratio.begin() + c * qn);
        }
    } else {
        // Almost no wall to measure: the plate as it is.
        std::fill(ratio.begin(), ratio.end(), 1.0f);
    }
    if (!transfer_.upload(ratio_q_, ratio)) return false;

    cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (!cmd) return false;
    const RelightParams rp{w, h, static_cast<Uint32>(qw), static_cast<Uint32>(qh)};
    dispatch(cmd, {&relight_, {&plate_, &ratio_q_}, {&wall_}, &rp, sizeof rp, nullptr, nullptr, w, h});
    return true;
}

bool SubjectGpu::foreground(SDL_GPUCommandBuffer*& cmd, int width, int height) {
    const std::size_t n = static_cast<std::size_t>(width) * height;
    const Uint32 w0 = static_cast<Uint32>(width), h0 = static_cast<Uint32>(height);

    // The mean colours of the solid subject and the clear background, summed
    // a row at a time on the GPU and the rows added here.
    if (!sums_.reserve(device_, static_cast<std::size_t>(height) * 8 * sizeof(float))) return false;
    const Size2 sp{w0, h0};
    dispatch(cmd, {&row_sums_, {&lin_, &alpha_}, {&sums_}, &sp, sizeof sp, nullptr, nullptr, h0, 1});
    if (!submit_and_wait(device_, cmd)) return false;
    cmd = nullptr;
    std::vector<float> sums(static_cast<std::size_t>(height) * 8);
    if (!transfer_.download(sums_, sums)) return false;
    double f_sum[4] = {}, b_sum[4] = {};
    for (int y = 0; y < height; ++y)
        for (int k = 0; k < 4; ++k) {
            f_sum[k] += sums[y * 8 + k];
            b_sum[k] += sums[y * 8 + 4 + k];
        }
    float f_mean[3], b_mean[3];
    for (int c = 0; c < 3; ++c) {
        f_mean[c] = static_cast<float>(f_sum[c] / (f_sum[3] + 1e-5));
        b_mean[c] = static_cast<float>(b_sum[c] / (b_sum[3] + 1e-5));
    }

    for (Buffer* b : {&f_[0], &f_[1], &b_[0], &b_[1]})
        if (!b->reserve(device_, n * 3 * sizeof(float))) return false;
    if (!img_l_.reserve(device_, n * 3 * sizeof(float)) || !a_l_.reserve(device_, n * sizeof(float)))
        return false;
    // The coarsest level, one pixel: the means.
    if (!transfer_.upload(f_[0], f_mean, sizeof f_mean) || !transfer_.upload(b_[0], b_mean, sizeof b_mean))
        return false;

    cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (!cmd) return false;
    constexpr int kSmallIterations = 10, kBigIterations = 2, kSmallSize = 32;
    const int levels = static_cast<int>(std::ceil(std::log2(static_cast<double>(std::max(width, height)))));
    int cur = 0;
    Uint32 wp = 1, hp = 1;
    for (int level = 0; level <= levels; ++level) {
        // Python's round(), halves to even -- as the CPU version.
        const Uint32 w = static_cast<Uint32>(
            std::nearbyint(std::pow(static_cast<double>(width), static_cast<double>(level) / levels)));
        const Uint32 h = static_cast<Uint32>(
            std::nearbyint(std::pow(static_cast<double>(height), static_cast<double>(level) / levels)));
        const Buffer* img = &lin_;
        const Buffer* a = &alpha_;
        if (w != w0 || h != h0) {
            const ResizeParams ri{w, h, w0, h0, 3};
            dispatch(cmd, {&resize_, {&lin_}, {&img_l_}, &ri, sizeof ri, nullptr, nullptr, w, h});
            const ResizeParams ra{w, h, w0, h0, 1};
            dispatch(cmd, {&resize_, {&alpha_}, {&a_l_}, &ra, sizeof ra, nullptr, nullptr, w, h});
            img = &img_l_;
            a = &a_l_;
        }
        const ResizeParams rf{w, h, wp, hp, 3};
        dispatch(cmd, {&resize_, {&f_[cur]}, {&f_[1 - cur]}, &rf, sizeof rf, nullptr, nullptr, w, h});
        dispatch(cmd, {&resize_, {&b_[cur]}, {&b_[1 - cur]}, &rf, sizeof rf, nullptr, nullptr, w, h});
        cur = 1 - cur;
        const int iterations = (w <= kSmallSize && h <= kSmallSize) ? kSmallIterations : kBigIterations;
        for (int it = 0; it < iterations; ++it)
            for (Uint32 parity = 0; parity < 2; ++parity) {
                const UpdateParams up{w, h, parity};
                dispatch(cmd, {&update_, {img, a}, {&f_[cur], &b_[cur]}, &up, sizeof up, nullptr, nullptr, w, h});
            }
        wp = w;
        hp = h;
    }
    // Left in f_[0] for the pack, whichever buffer the last level used.
    if (cur != 0) std::swap(f_[0], f_[1]), std::swap(b_[0], b_[1]);
    return true;
}

void SubjectGpu::blur(SDL_GPUCommandBuffer* cmd, Buffer& planes, Buffer& scratch, int width, int height, int depth,
                      float sigma) {
    BlurParams p = {};
    p.width = static_cast<Uint32>(width);
    p.height = static_cast<Uint32>(height);
    p.depth = static_cast<Uint32>(depth);
    p.radius = std::min(static_cast<int>(4.0 * sigma + 0.5), 33);
    double sum = 0.0;
    double k[68] = {};
    for (int i = -p.radius; i <= p.radius; ++i) sum += k[i + p.radius] = std::exp(-0.5 * i * i / (double(sigma) * sigma));
    for (int i = 0; i <= 2 * p.radius; ++i) p.weights[i] = static_cast<float>(k[i] / sum);
    // Down the columns, then along the rows, as scipy and the CPU version.
    p.vertical = 1;
    dispatch(cmd, {&blur_, {&planes}, {&scratch}, &p, sizeof p, nullptr, nullptr, p.width, p.height});
    p.vertical = 0;
    dispatch(cmd, {&blur_, {&scratch}, {&planes}, &p, sizeof p, nullptr, nullptr, p.width, p.height});
}

}  // namespace ps::gpu

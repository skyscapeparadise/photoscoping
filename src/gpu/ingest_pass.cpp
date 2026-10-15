#include "gpu/ingest_pass.hpp"

#include <cstring>

#include "color/cube_lut.hpp"
#include "gpu/pipeline.hpp"
#include "gpu/shaders_generated.hpp"
#include "media/video_decoder.hpp"

namespace ps::gpu {

InputTransform transform_from_tags(const media::SignalInfo& s) {
    InputTransform t;
    t.matrix = s.matrix;
    t.range = s.range;
    t.gamut = s.primaries == "bt2020" ? color::Gamut::kRec2020 : color::Gamut::kRec709;
    if (s.transfer == "smpte2084") t.transfer = color::Transfer::kPq;
    else if (s.transfer == "arib-std-b67") t.transfer = color::Transfer::kHlg;
    else if (s.transfer == "linear") t.transfer = color::Transfer::kLinear;
    else if (s.transfer == "iec61966-2-1") t.transfer = color::Transfer::kSrgb;
    else t.transfer = color::Transfer::kBt1886;
    return t;
}

namespace {

// Linear light needs float, and half float is enough: 11 bits of mantissa
// against the 10-bit sources, with range far past any HDR highlight.
constexpr SDL_GPUTextureFormat kWorkingFormat = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;

// std140 layout of ingest.frag's Ingest block.
struct IngestUniform {
    float ycbcr[3][4];
    float to_working[3][4];
    float lut_domain_min[4];
    float lut_domain_max[4];
    float sample_scale;
    float interleaved;
    float transfer;
    float pad;
};

float half_to_float(std::uint16_t h) {
    const std::uint32_t sign = static_cast<std::uint32_t>(h & 0x8000) << 16;
    std::uint32_t exponent = (h >> 10) & 0x1F;
    std::uint32_t mantissa = h & 0x3FF;
    std::uint32_t bits;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            // Subnormal: normalise it.
            exponent = 127 - 15 + 1;
            while (!(mantissa & 0x400)) {
                mantissa <<= 1;
                --exponent;
            }
            bits = sign | (exponent << 23) | ((mantissa & 0x3FF) << 13);
        }
    } else if (exponent == 31) {
        bits = sign | 0x7F800000 | (mantissa << 13);
    } else {
        bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
    }
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

SDL_GPUTexture* make_texture(SDL_GPUDevice* device, SDL_GPUTextureType type,
                             SDL_GPUTextureFormat format, Uint32 w, Uint32 h, Uint32 depth,
                             SDL_GPUTextureUsageFlags usage) {
    SDL_GPUTextureCreateInfo info = {};
    info.type = type;
    info.format = format;
    info.usage = usage;
    info.width = w;
    info.height = h;
    info.layer_count_or_depth = depth;
    info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture* texture = SDL_CreateGPUTexture(device, &info);
    if (!texture) SDL_Log("SDL_CreateGPUTexture failed: %s", SDL_GetError());
    return texture;
}

// Uploads `bytes` into a whole texture and waits for it. For one-off uploads
// (LUTs), not per-frame data.
bool upload_once(SDL_GPUDevice* device, SDL_GPUTexture* texture, const void* bytes, Uint32 size,
                 Uint32 w, Uint32 h, Uint32 d) {
    SDL_GPUTransferBufferCreateInfo info = {};
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    info.size = size;
    SDL_GPUTransferBuffer* buffer = SDL_CreateGPUTransferBuffer(device, &info);
    if (!buffer) return false;
    void* mapped = SDL_MapGPUTransferBuffer(device, buffer, false);
    std::memcpy(mapped, bytes, size);
    SDL_UnmapGPUTransferBuffer(device, buffer);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src = {};
    src.transfer_buffer = buffer;
    src.pixels_per_row = w;
    src.rows_per_layer = h;
    SDL_GPUTextureRegion dst = {};
    dst.texture = texture;
    dst.w = w;
    dst.h = h;
    dst.d = d;
    SDL_UploadToGPUTexture(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence) {
        SDL_WaitForGPUFences(device, true, &fence, 1);
        SDL_ReleaseGPUFence(device, fence);
    }
    SDL_ReleaseGPUTransferBuffer(device, buffer);
    return fence != nullptr;
}

}  // namespace

IngestPass::~IngestPass() { destroy(); }

bool IngestPass::create(SDL_GPUDevice* device) {
    device_ = device;
    pipeline_ = create_quad_pipeline(device, shaders::kIngestFragment, 4, kWorkingFormat);
    linear_ = create_sampler(device, SDL_GPU_FILTER_LINEAR);
    nearest_ = create_sampler(device, SDL_GPU_FILTER_NEAREST);
    no_lut_ = make_texture(device, SDL_GPU_TEXTURETYPE_3D, SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT,
                           1, 1, 1, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    if (!pipeline_ || !linear_ || !nearest_ || !no_lut_) return false;
    const float zero[4] = {0, 0, 0, 0};
    return upload_once(device, no_lut_, zero, sizeof zero, 1, 1, 1);
}

void IngestPass::release_frame_textures() {
    for (auto*& plane : planes_) {
        if (plane) SDL_ReleaseGPUTexture(device_, plane);
        plane = nullptr;
    }
    if (working_) SDL_ReleaseGPUTexture(device_, working_);
    working_ = nullptr;
    if (upload_) SDL_ReleaseGPUTransferBuffer(device_, upload_);
    upload_ = nullptr;
    width_ = height_ = 0;
}

void IngestPass::destroy() {
    if (!device_) return;
    release_frame_textures();
    if (lut_texture_) SDL_ReleaseGPUTexture(device_, lut_texture_);
    if (no_lut_) SDL_ReleaseGPUTexture(device_, no_lut_);
    if (linear_) SDL_ReleaseGPUSampler(device_, linear_);
    if (nearest_) SDL_ReleaseGPUSampler(device_, nearest_);
    if (pipeline_) SDL_ReleaseGPUGraphicsPipeline(device_, pipeline_);
    lut_texture_ = no_lut_ = nullptr;
    linear_ = nearest_ = nullptr;
    pipeline_ = nullptr;
    device_ = nullptr;
}

bool IngestPass::set_lut(const color::CubeLut& lut) {
    if (!lut.is_3d) {
        SDL_Log("ingest: 1D LUTs are not supported yet");
        return false;
    }
    const Uint32 n = static_cast<Uint32>(lut.size);
    // RGB -> RGBA: there is no three-channel float texture format.
    std::vector<float> rgba(static_cast<std::size_t>(n) * n * n * 4);
    for (std::size_t i = 0, count = rgba.size() / 4; i < count; ++i) {
        rgba[i * 4] = lut.data[i * 3];
        rgba[i * 4 + 1] = lut.data[i * 3 + 1];
        rgba[i * 4 + 2] = lut.data[i * 3 + 2];
        rgba[i * 4 + 3] = 1.0f;
    }
    SDL_GPUTexture* texture =
        make_texture(device_, SDL_GPU_TEXTURETYPE_3D, SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT, n,
                     n, n, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    if (!texture) return false;
    if (!upload_once(device_, texture, rgba.data(),
                     static_cast<Uint32>(rgba.size() * sizeof(float)), n, n, n)) {
        SDL_ReleaseGPUTexture(device_, texture);
        return false;
    }
    if (lut_texture_) SDL_ReleaseGPUTexture(device_, lut_texture_);
    lut_texture_ = texture;
    lut_size_ = static_cast<float>(n);
    for (int c = 0; c < 3; ++c) {
        lut_min_[c] = lut.domain_min[c];
        lut_max_[c] = lut.domain_max[c];
    }
    return true;
}

bool IngestPass::ensure_textures(const media::VideoDecoder& frame) {
    const media::FrameLayout& l = frame.layout();
    const Uint32 w = static_cast<Uint32>(l.width), h = static_cast<Uint32>(l.height);
    const Uint32 cw = static_cast<Uint32>(l.chroma_width);
    const Uint32 ch = static_cast<Uint32>(l.chroma_height);
    if (working_ && w == width_ && h == height_ && cw == chroma_width_ && ch == chroma_height_ &&
        l.bytes_per_sample == bytes_per_sample_ && l.interleaved_chroma == interleaved_)
        return true;

    release_frame_textures();
    const bool wide = l.bytes_per_sample == 2;
    const SDL_GPUTextureFormat one = wide ? SDL_GPU_TEXTUREFORMAT_R16_UNORM
                                          : SDL_GPU_TEXTUREFORMAT_R8_UNORM;
    const SDL_GPUTextureFormat two = wide ? SDL_GPU_TEXTUREFORMAT_R16G16_UNORM
                                          : SDL_GPU_TEXTUREFORMAT_R8G8_UNORM;
    const auto sampler = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    planes_[0] = make_texture(device_, SDL_GPU_TEXTURETYPE_2D, one, w, h, 1, sampler);
    planes_[1] = make_texture(device_, SDL_GPU_TEXTURETYPE_2D, l.interleaved_chroma ? two : one,
                              cw, ch, 1, sampler);
    // The interleaved layout has no third plane, but the shader still
    // declares one; a 1x1 stand-in keeps the binding valid.
    planes_[2] = l.interleaved_chroma
                     ? make_texture(device_, SDL_GPU_TEXTURETYPE_2D, one, 1, 1, 1, sampler)
                     : make_texture(device_, SDL_GPU_TEXTURETYPE_2D, one, cw, ch, 1, sampler);
    working_ = make_texture(device_, SDL_GPU_TEXTURETYPE_2D, kWorkingFormat, w, h, 1,
                            SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);

    const Uint32 luma = w * h * l.bytes_per_sample;
    const Uint32 chroma = cw * ch * l.bytes_per_sample * (l.interleaved_chroma ? 2 : 1);
    upload_size_ = luma + chroma * (l.interleaved_chroma ? 1 : 2);
    SDL_GPUTransferBufferCreateInfo info = {};
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    info.size = upload_size_;
    upload_ = SDL_CreateGPUTransferBuffer(device_, &info);
    if (!planes_[0] || !planes_[1] || !planes_[2] || !working_ || !upload_) {
        release_frame_textures();
        return false;
    }
    width_ = w;
    height_ = h;
    chroma_width_ = cw;
    chroma_height_ = ch;
    bytes_per_sample_ = l.bytes_per_sample;
    interleaved_ = l.interleaved_chroma;
    return true;
}

bool IngestPass::process(SDL_GPUCommandBuffer* cmd, const media::VideoDecoder& frame,
                         const InputTransform& transform) {
    if (!ensure_textures(frame)) return false;
    const media::FrameLayout& l = frame.layout();

    // Pack the planes tightly: the decoder's rows carry alignment padding.
    // cycle=true gives a fresh buffer if the GPU is still reading the last one.
    auto* mapped = static_cast<std::uint8_t*>(SDL_MapGPUTransferBuffer(device_, upload_, true));
    if (!mapped) return false;
    struct Plane {
        Uint32 w, h, row_bytes, offset;
    };
    const int plane_count = l.interleaved_chroma ? 2 : 3;
    Plane planes[3];
    Uint32 offset = 0;
    for (int p = 0; p < plane_count; ++p) {
        const Uint32 w = p == 0 ? width_ : chroma_width_;
        const Uint32 h = p == 0 ? height_ : chroma_height_;
        const Uint32 row = w * l.bytes_per_sample * (p == 1 && l.interleaved_chroma ? 2 : 1);
        const std::uint8_t* src = frame.plane(p);
        for (Uint32 y = 0; y < h; ++y)
            std::memcpy(mapped + offset + y * row, src + static_cast<std::size_t>(y) * frame.stride(p),
                        row);
        planes[p] = {w, h, row, offset};
        offset += row * h;
    }
    SDL_UnmapGPUTransferBuffer(device_, upload_);

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    for (int p = 0; p < plane_count; ++p) {
        SDL_GPUTextureTransferInfo src = {};
        src.transfer_buffer = upload_;
        src.offset = planes[p].offset;
        src.pixels_per_row = planes[p].w;
        src.rows_per_layer = planes[p].h;
        SDL_GPUTextureRegion dst = {};
        dst.texture = planes_[p];
        dst.w = planes[p].w;
        dst.h = planes[p].h;
        dst.d = 1;
        SDL_UploadToGPUTexture(copy, &src, &dst, true);
    }
    SDL_EndGPUCopyPass(copy);

    IngestUniform u = {};
    const color::Affine3x4 m = color::ycbcr_to_rgb(transform.matrix, transform.range, l.bit_depth);
    const color::Mat3 g = color::to_working(transform.gamut);
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) u.ycbcr[r][c] = m[r * 4 + c];
        for (int c = 0; c < 3; ++c) u.to_working[r][c] = g[r * 3 + c];
    }
    const bool lut = transform.use_lut && lut_texture_;
    for (int c = 0; c < 3; ++c) {
        u.lut_domain_min[c] = lut_min_[c];
        u.lut_domain_max[c] = lut_max_[c];
    }
    u.lut_domain_min[3] = lut ? lut_size_ : 0.0f;
    u.sample_scale = l.sample_scale;
    u.interleaved = l.interleaved_chroma ? 1.0f : 0.0f;
    u.transfer = static_cast<float>(static_cast<int>(transform.transfer));

    SDL_GPUColorTargetInfo target = {};
    target.texture = working_;
    target.load_op = SDL_GPU_LOADOP_DONT_CARE;
    target.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(pass, pipeline_);
    const SDL_GPUTextureSamplerBinding bindings[4] = {
        {planes_[0], linear_},
        {planes_[1], linear_},
        {planes_[2], linear_},
        {lut ? lut_texture_ : no_lut_, nearest_},
    };
    SDL_BindGPUFragmentSamplers(pass, 0, bindings, 4);
    SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof u);
    draw_quad(cmd, pass, QuadRect{});
    SDL_EndGPURenderPass(pass);
    return true;
}

std::vector<std::uint16_t> IngestPass::read_working_raw() {
    std::vector<std::uint16_t> out;
    if (!working_) return out;
    const Uint32 size = width_ * height_ * 8;
    SDL_GPUTransferBufferCreateInfo info = {};
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    info.size = size;
    SDL_GPUTransferBuffer* buffer = SDL_CreateGPUTransferBuffer(device_, &info);
    if (!buffer) return out;

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion src = {};
    src.texture = working_;
    src.w = width_;
    src.h = height_;
    src.d = 1;
    SDL_GPUTextureTransferInfo dst = {};
    dst.transfer_buffer = buffer;
    dst.pixels_per_row = width_;
    dst.rows_per_layer = height_;
    SDL_DownloadFromGPUTexture(copy, &src, &dst);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence) {
        SDL_WaitForGPUFences(device_, true, &fence, 1);
        SDL_ReleaseGPUFence(device_, fence);
        const auto* halves = static_cast<const std::uint16_t*>(
            SDL_MapGPUTransferBuffer(device_, buffer, false));
        out.assign(halves, halves + static_cast<std::size_t>(width_) * height_ * 4);
        SDL_UnmapGPUTransferBuffer(device_, buffer);
    }
    SDL_ReleaseGPUTransferBuffer(device_, buffer);
    return out;
}

std::vector<float> IngestPass::read_working() {
    const std::vector<std::uint16_t> halves = read_working_raw();
    std::vector<float> out(halves.size());
    for (std::size_t i = 0; i < halves.size(); ++i) out[i] = half_to_float(halves[i]);
    return out;
}

}  // namespace ps::gpu

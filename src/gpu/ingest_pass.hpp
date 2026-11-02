#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <vector>

#include "color/color_space.hpp"
#include "color/ycbcr.hpp"

namespace ps::color {
struct CubeLut;
}
namespace ps::media {
class VideoDecoder;
struct SignalInfo;
}

namespace ps::gpu {

// How a clip gets from its decoded signal into the working space.
struct InputTransform {
    // The R'G'B' signal is decoded with these. When `lut` is set, they
    // describe the LUT's *output*; otherwise the file's own tags.
    color::YcbcrMatrix matrix = color::YcbcrMatrix::kBt709;
    color::SignalRange range = color::SignalRange::kLimited;
    color::Gamut gamut = color::Gamut::kRec709;
    color::Transfer transfer = color::Transfer::kBt1886;
    bool use_lut = false;
};

// What a file's own tags say, for a clip with no LUT in front of it.
InputTransform transform_from_tags(const media::SignalInfo& signal);
// The transform for a clip seen through `lut`, or through its tags when that
// is null.
InputTransform transform_for(const media::SignalInfo& signal, const color::CubeLut* lut);
// The clip as the light that fell on the camera, no LUT: decoded from the
// camera log the LUT says it expects (Canon's do), or by the file's tags.
// Whether it found a log to decode is in `from_log`.
InputTransform scene_transform_for(const media::SignalInfo& signal, const color::CubeLut* lut,
                                   bool* from_log = nullptr);

// Uploads a decoded frame's planes and converts them into the working image:
// linear light, Rec.2020, 1.0 = reference white, at the source's resolution.
// Everything downstream -- correction, matting, display -- reads that image.
class IngestPass {
public:
    IngestPass() = default;
    ~IngestPass();
    IngestPass(const IngestPass&) = delete;
    IngestPass& operator=(const IngestPass&) = delete;

    bool create(SDL_GPUDevice* device);
    void destroy();

    // Uploads a 3D LUT (1D is refused for now). Replaces any previous one.
    bool set_lut(const color::CubeLut& lut);
    bool has_lut() const { return lut_texture_ != nullptr; }

    // Records the plane upload and the conversion into `cmd`.
    bool process(SDL_GPUCommandBuffer* cmd, const media::VideoDecoder& frame,
                 const InputTransform& transform);

    // The planes process() last uploaded, converted again with another
    // transform into a second image -- the same frame seen two ways: through
    // the LUT for matting and the eye, and as scene light for light maps.
    bool process_again(SDL_GPUCommandBuffer* cmd, const InputTransform& transform);
    SDL_GPUTexture* scene() const { return scene_; }
    std::vector<float> read_scene();
    std::vector<std::uint16_t> read_scene_raw();

    SDL_GPUTexture* working() const { return working_; }
    Uint32 width() const { return width_; }
    Uint32 height() const { return height_; }

    // Reads the working image back, RGBA as 32-bit floats. Synchronous: it
    // submits and waits, so it is for probes and tests, not the frame loop.
    std::vector<float> read_working();
    // The same, as the half floats the texture stores, without conversion.
    std::vector<std::uint16_t> read_working_raw();

private:
    bool convert(SDL_GPUCommandBuffer* cmd, const InputTransform& transform, SDL_GPUTexture* into);
    std::vector<std::uint16_t> read_raw(SDL_GPUTexture* texture);
    bool ensure_textures(const media::VideoDecoder& frame);
    void release_frame_textures();

    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUGraphicsPipeline* pipeline_ = nullptr;
    SDL_GPUSampler* linear_ = nullptr;
    SDL_GPUSampler* nearest_ = nullptr;
    SDL_GPUTexture* planes_[3] = {nullptr, nullptr, nullptr};
    SDL_GPUTransferBuffer* upload_ = nullptr;
    Uint32 upload_size_ = 0;
    SDL_GPUTexture* working_ = nullptr;
    SDL_GPUTexture* scene_ = nullptr;
    // The uploaded planes' layout, for converting them again.
    int bit_depth_ = 8;
    float sample_scale_ = 1.0f;
    bool uploaded_ = false;
    SDL_GPUTexture* lut_texture_ = nullptr;
    // Bound in place of a LUT when there is none, because every sampler slot
    // a shader declares must be bound.
    SDL_GPUTexture* no_lut_ = nullptr;
    float lut_size_ = 0.0f;
    float lut_min_[3] = {0, 0, 0};
    float lut_max_[3] = {1, 1, 1};

    Uint32 width_ = 0, height_ = 0;
    Uint32 chroma_width_ = 0, chroma_height_ = 0;
    int bytes_per_sample_ = 0;
    bool interleaved_ = false;
};

}  // namespace ps::gpu

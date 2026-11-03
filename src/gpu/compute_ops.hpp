#pragma once

// Image work on the GPU, chained so a frame stays there between steps.

#include <SDL3/SDL.h>

#include <cstdint>
#include <vector>

#include "gpu/compute.hpp"

namespace ps::gpu {

// The subject export's per-frame work: the frame (the ingest pass's working
// texture) and its final matte in, 16-bit straight-alpha RGBA out, Rec.709
// encoded -- what run_export hands the encoder. With a plate, the subject's
// colour comes from the wall behind it relit to the frame (relight_plate,
// then estimate_foreground_known); without, from estimate_foreground_ml.
//
// Matches the CPU versions closely rather than exactly: the foreground solve
// updates a checkerboard at a time where the CPU sweeps in order.
class SubjectGpu {
public:
    explicit SubjectGpu(SDL_GPUDevice* device);
    ~SubjectGpu();
    SubjectGpu(const SubjectGpu&) = delete;
    SubjectGpu& operator=(const SubjectGpu&) = delete;

    bool ok() const { return ok_; }

    // The clean plate, planar linear Rec.709 at the frame's size; empty for
    // none.
    bool set_plate(const std::vector<float>& linear, int width, int height);

    // `working` is the ingest pass's texture, linear Rec.2020; `alpha` the
    // final matte. `rgba` is resized to width * height * 4.
    bool finish(SDL_GPUTexture* working, const std::vector<float>& alpha, int width, int height,
                std::vector<std::uint16_t>* rgba);

    // The same from planar linear Rec.709 already in memory, for tests and
    // for frames that never reached the ingest pass.
    bool finish_linear(const std::vector<float>& linear, const std::vector<float>& alpha, int width,
                       int height, std::vector<std::uint16_t>* rgba);

private:
    bool run(const std::vector<float>& alpha, int width, int height, std::vector<std::uint16_t>* rgba);
    bool relight(SDL_GPUCommandBuffer*& cmd, int width, int height);
    bool foreground(SDL_GPUCommandBuffer*& cmd, int width, int height);
    void blur(SDL_GPUCommandBuffer* cmd, Buffer& planes, Buffer& scratch, int width, int height, int depth,
              float sigma);

    SDL_GPUDevice* device_;
    SDL_GPUSampler* sampler_ = nullptr;
    Transfer transfer_;
    bool ok_ = false;

    Kernel prepare_, dilate_, pool_, relight_, row_sums_, resize_, update_, direct_, blur_, pack_;

    bool has_plate_ = false;
    Buffer plate_;
    Buffer lin_, alpha_, near_a_, near_b_, ratio_q_, weight_q_, wall_;
    Buffer sums_, img_l_, a_l_, f_[2], b_[2], direct_planes_, scratch_, samples_;
};

// A light map's per-frame work on the GPU: the frame (the ingest pass's
// scene or working texture) converted to linear Rec.709 there, and each crop
// warped out of it -- with the matte and the plate -- along with the samples
// its surface is fitted to. Only crop-sized results come back.
class LightGpu {
public:
    explicit LightGpu(SDL_GPUDevice* device);
    ~LightGpu();
    LightGpu(const LightGpu&) = delete;
    LightGpu& operator=(const LightGpu&) = delete;

    bool ok() const { return ok_; }

    // The clean plate, planar linear Rec.709 at the frame's size; empty for
    // none.
    bool set_plate(const std::vector<float>& linear, int width, int height);
    bool has_plate() const { return has_plate_; }

    // The frame to crop from, and its matte (empty: none, all backdrop).
    bool begin_frame(SDL_GPUTexture* texture, const std::vector<float>& alpha, int width, int height);
    // The same from planar linear Rec.709 in memory, for tests.
    bool begin_frame_linear(const std::vector<float>& linear, const std::vector<float>& alpha, int width,
                            int height);

    // The light -- over the plate, for `ratio` -- at a grid's points: every
    // `stride` pixels from (x0, y0), columns x rows of them, three planes.
    bool gather(int x0, int y0, int stride, int columns, int rows, bool ratio, std::vector<float>* values);

    // One crop, width x height, through the homography `m` (row by row) with
    // `samples` x `samples` samples a pixel: light (3 planes), matte, and the
    // plate (3 planes) when `with_plate`.
    bool warp(const double m[9], int samples, int width, int height, bool with_plate, std::vector<float>* rgb,
              std::vector<float>* alpha, std::vector<float>* plate);

private:
    SDL_GPUDevice* device_;
    SDL_GPUSampler* sampler_ = nullptr;
    Transfer transfer_;
    bool ok_ = false;
    Kernel prepare_, gather_, warp_;
    bool has_plate_ = false;
    int width_ = 0, height_ = 0;
    Buffer plate_, lin_, alpha_, values_, rgb_, crop_alpha_, crop_plate_;
};

}  // namespace ps::gpu

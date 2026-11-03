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

}  // namespace ps::gpu

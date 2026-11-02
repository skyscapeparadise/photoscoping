#pragma once

#include <string>
#include <vector>

#include "matting/matte_model.hpp"
#include "matting/preprocess.hpp"

namespace ps::matting {

// The matting stack, natively, stage by stage as tools/matting_stack.py has
// it, so each stage can be checked against the Python one:
//
//   1. coarse  the model on the whole frame at 2048x2048, which decides what
//              the subject is, scaled back to the frame's size;
//   2. tiled   the model again on 2048x1152 tiles at full resolution, trusted
//              only in the coarse matte's soft band, for edge detail;
//   3. band    closed-form matting in that band, at full resolution, where
//              subject and wall differ enough in colour for it to mean
//              anything -- the shape of Photoshop's Refine Hair;
//   4. subject the wall's share taken out of the edge colours, giving the
//              subject's own light, premultiplied by its alpha.
//
// Alphas are row-major floats at the frame's size, 0..1. The subject is
// planar linear Rec.709, the same size.
class MattingStack {
public:
    // Loads both models from `model_dir`, where tools/export_matting_model.py
    // writes them.
    bool load(const std::string& model_dir, MatteModel::Backend backend,
              const std::string& cache_dir, std::string* error);

    std::vector<float> coarse(const MattingFrame& frame, std::string* error);
    std::vector<float> tiled(const MattingFrame& frame, const std::vector<float>& coarse,
                             std::string* error);
    // With `wall` -- a clean plate relit for this frame (matting/plate.hpp),
    // planar linear Rec.709 -- the band knows the wall instead of guessing it:
    // a pixel in it the matte calls mostly wall that matches the plate within
    // `tolerance` is solved as wall, and the wall's true colour says how far
    // the solve can be trusted.
    //
    // `scale` widens its pixel distances (resolution_scale()); 0 takes it
    // from the frame's own size, which is wrong for a crop -- the closer look
    // passes the whole frame's.
    static std::vector<float> band(const MattingFrame& frame, const std::vector<float>& tiled,
                                   std::string* error, const std::vector<float>* wall = nullptr,
                                   float tolerance = 0.0f, float scale = 0.0f);

    // How much the stack's distances in pixels -- how far the edge band
    // reaches, how soft its own edge is -- are widened for a frame this size.
    // They were set on 4K, where the coarse model sees the frame at half its
    // size; at 8K it sees a quarter, and its edges are twice as soft in the
    // frame's pixels. 1 at 4K and below, which are left exactly as they were.
    static float resolution_scale(int width, int height);
    static std::vector<float> subject(const MattingFrame& frame, const std::vector<float>& alpha);

    // A local re-matte: the region from (x, y), w x h, looked at again on its
    // own, magnified to fill a model -- the 2048x1152 one if `wide`, else
    // the square one -- then refined as the band stage refines a frame.
    //
    // A crop shows the model part of the subject, not the whole of it, so as
    // with the tiles its answer is trusted for edge detail and not for what
    // the subject is: only where it or `base` is soft, widened and feathered.
    // Everywhere else the crop keeps `base`, the frame's own matte, so the
    // result meets the matte around it without a seam. Returns the region's
    // alpha, w x h.
    // `wall`, if given, is the frame's relit plate, as band() takes it.
    std::vector<float> closer(const MattingFrame& frame, const std::vector<float>& base, int x,
                              int y, int w, int h, bool wide, std::string* error,
                              const std::vector<float>* wall = nullptr, float tolerance = 0.0f);

private:
    MatteModel whole_;
    MatteModel tile_;
};

// Where tiles start along one side: the fewest evenly spaced ones that cover
// `length` while still overlapping by `min_overlap`, enough to feather across.
// matting_stack.tile_starts; UHD takes two each way.
std::vector<int> tile_starts(int length, int tile, int min_overlap = 128);

}  // namespace ps::matting

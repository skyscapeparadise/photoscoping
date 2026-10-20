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
    static std::vector<float> band(const MattingFrame& frame, const std::vector<float>& tiled,
                                   std::string* error);
    static std::vector<float> subject(const MattingFrame& frame, const std::vector<float>& alpha);

private:
    MatteModel whole_;
    MatteModel tile_;
};

// Where tiles start along one side: the fewest evenly spaced ones that cover
// `length` while still overlapping by `min_overlap`, enough to feather across.
// matting_stack.tile_starts; UHD takes two each way.
std::vector<int> tile_starts(int length, int tile, int min_overlap = 128);

}  // namespace ps::matting

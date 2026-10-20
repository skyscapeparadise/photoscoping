#pragma once

#include <cstdint>
#include <vector>

namespace ps::matting {

// The frame as the matting stack reads it, made once and shared by every
// stage: Rec.709 both as the model sees it -- sRGB-encoded, 0..1 -- and as
// light, linear and unclamped, for the stages that separate the subject's
// light from the wall's. Planar -- all of red, then green, then blue -- at the
// frame's own size.
struct MattingFrame {
    std::vector<float> planes;  // sRGB
    std::vector<float> linear;
    int width = 0;
    int height = 0;

    const float* plane(int c) const {
        return &planes[static_cast<std::size_t>(c) * width * height];
    }
    const float* linear_plane(int c) const {
        return &linear[static_cast<std::size_t>(c) * width * height];
    }
};

// From the working image as read back: width x height, four floats a pixel,
// linear Rec.2020.
MattingFrame prepare_frame(const std::vector<float>& rgba, int width, int height);

// The model's input for the whole frame, made exactly as the Python stack
// makes it (tools/matting_bakeoff.py, TorchMethod.__call__): resized to
// out_w x out_h by PyTorch's bicubic and normalised by the ImageNet mean and
// deviation, planar as the model takes it.
//
// The resize is PyTorch's own to the letter -- the cubic with a = -0.75,
// pixel centres not corners, the source edge clamped, no antialiasing on the
// way down -- because the model was only ever shown frames made that way.
// tools/onnx_spike.py holds frames prepared in Python to compare against.
std::vector<float> model_input(const MattingFrame& frame, int out_w, int out_h);

// A tile's input: the region at the frame's own resolution, normalised.
// Where it runs past the frame's edge the edge pixel repeats, as np.pad's
// "edge" does for a frame smaller than a tile.
std::vector<float> tile_input(const MattingFrame& frame, int x, int y, int w, int h);

}  // namespace ps::matting

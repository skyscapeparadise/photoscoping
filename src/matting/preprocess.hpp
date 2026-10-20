#pragma once

#include <cstdint>
#include <vector>

namespace ps::matting {

// The model's input, made from the working image exactly as the Python stack
// makes it (tools/matting_bakeoff.py, TorchMethod.__call__): Rec.2020 linear
// to Rec.709, sRGB-encoded, resized to size x size by PyTorch's bicubic, and
// normalised by the ImageNet mean and deviation. Planar, as the model takes
// it: all of red, then green, then blue.
//
// The resize is PyTorch's own to the letter -- the cubic with a = -0.75,
// pixel centres not corners, the source edge clamped, no antialiasing on the
// way down -- because the model was only ever shown frames made that way.
// tools/onnx_spike.py holds frames prepared in Python to compare against.
//
// `rgba` is the working image as read back: width x height, four floats a
// pixel.
std::vector<float> model_input(const std::vector<float>& rgba, int width, int height, int size);

}  // namespace ps::matting

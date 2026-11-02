#pragma once

#include <string>
#include <vector>

namespace ps::media {

// Writes an OpenEXR image: RGB, half float, scanline, ZIP compression (16
// lines a block, as OpenEXR's own writer does) -- what Blender and every
// compositor read, written without the OpenEXR library.
//
// `rgb` is planar linear light, width x height each, values as they are:
// nothing is clamped.
bool write_exr_half(const std::string& path, const std::vector<float>& rgb, int width, int height);

}  // namespace ps::media

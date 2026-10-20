#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ps::matting {

// Writes a 16-bit greyscale PNG: a matte at the precision the stack makes it.
// SDL_image writes eight bits a channel and no more, which bands a soft edge;
// this is the smallest writer that does sixteen, on zlib.
//
// `values` are 0..1, width x height, row-major; they are rounded to 0..65535,
// as tools/matte_clip.py does.
bool write_png16(const std::string& path, const std::vector<float>& values, int width, int height);

}  // namespace ps::matting

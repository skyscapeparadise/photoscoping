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

// 16-bit RGBA, interleaved samples as written: for exported frames.
bool write_png16_rgba(const std::string& path, const std::vector<std::uint16_t>& rgba, int width,
                      int height);

// 0..1 to a 16-bit sample, rounded.
std::uint16_t to_sample(float v);

// Reads a greyscale PNG, 8 or 16 bits, at full precision -- SDL_image would
// bring a 16-bit matte down to 8. False for anything else.
bool read_png16(const std::string& path, std::vector<float>* values, int* width, int* height);

}  // namespace ps::matting

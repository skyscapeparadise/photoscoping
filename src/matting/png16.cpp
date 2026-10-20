#include "matting/png16.hpp"

#include <zlib.h>

#include <algorithm>
#include <cstdio>

namespace ps::matting {

namespace {

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

// A chunk: length, type, data, and the CRC of type and data.
void chunk(std::vector<std::uint8_t>& out, const char type[4], const std::uint8_t* data,
           std::size_t size) {
    put32(out, static_cast<std::uint32_t>(size));
    const std::size_t at = out.size();
    out.insert(out.end(), type, type + 4);
    if (size) out.insert(out.end(), data, data + size);
    put32(out, static_cast<std::uint32_t>(
                   crc32(0, out.data() + at, static_cast<uInt>(size + 4))));
}

}  // namespace

bool write_png16(const std::string& path, const std::vector<float>& values, int width, int height) {
    // Each row: filter type 1 (sub), which on a matte -- long runs of 0 and
    // of 65535 -- leaves mostly zeros for deflate to fold away. Two bytes a
    // sample, big-endian, as PNG has them.
    const std::size_t row_bytes = 1 + static_cast<std::size_t>(width) * 2;
    std::vector<std::uint8_t> raw(row_bytes * height);
    std::vector<std::uint8_t> row(static_cast<std::size_t>(width) * 2);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float v = std::clamp(values[static_cast<std::size_t>(y) * width + x], 0.0f, 1.0f);
            const auto s = static_cast<std::uint16_t>(v * 65535.0f + 0.5f);
            row[x * 2] = static_cast<std::uint8_t>(s >> 8);
            row[x * 2 + 1] = static_cast<std::uint8_t>(s);
        }
        std::uint8_t* dst = &raw[y * row_bytes];
        dst[0] = 1;
        for (std::size_t i = 0; i < row.size(); ++i)
            dst[1 + i] = static_cast<std::uint8_t>(row[i] - (i >= 2 ? row[i - 2] : 0));
    }

    uLongf packed_size = compressBound(static_cast<uLong>(raw.size()));
    std::vector<std::uint8_t> packed(packed_size);
    if (compress2(packed.data(), &packed_size, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK)
        return false;

    std::vector<std::uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<std::uint8_t> header;
    put32(header, static_cast<std::uint32_t>(width));
    put32(header, static_cast<std::uint32_t>(height));
    header.insert(header.end(), {16, 0, 0, 0, 0});  // 16-bit, greyscale, defaults
    chunk(out, "IHDR", header.data(), header.size());
    chunk(out, "IDAT", packed.data(), packed_size);
    chunk(out, "IEND", nullptr, 0);

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool wrote = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    return std::fclose(f) == 0 && wrote;
}

}  // namespace ps::matting

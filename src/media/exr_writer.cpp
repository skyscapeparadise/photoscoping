#include "media/exr_writer.hpp"

#include <zlib.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "media/half.hpp"

namespace ps::media {

namespace {

void put(std::vector<std::uint8_t>& out, const void* data, std::size_t size) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    out.insert(out.end(), p, p + size);
}
void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void put64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void put_float(std::vector<std::uint8_t>& out, float f) {
    std::uint32_t bits;
    std::memcpy(&bits, &f, 4);
    put32(out, bits);
}
void attribute(std::vector<std::uint8_t>& out, const char* name, const char* type,
               const std::vector<std::uint8_t>& value) {
    put(out, name, std::strlen(name) + 1);
    put(out, type, std::strlen(type) + 1);
    put32(out, static_cast<std::uint32_t>(value.size()));
    put(out, value.data(), value.size());
}

constexpr int kLinesPerBlock = 16;  // ZIP_COMPRESSION

}  // namespace

bool write_exr_half(const std::string& path, const std::vector<float>& rgb, int width, int height) {
    const std::size_t n = static_cast<std::size_t>(width) * height;
    if (width <= 0 || height <= 0 || rgb.size() != n * 3) return false;

    std::vector<std::uint8_t> out = {0x76, 0x2f, 0x31, 0x01, 2, 0, 0, 0};  // magic, version 2
    // Channels, in alphabetical order as the format requires: B, G, R.
    std::vector<std::uint8_t> channels;
    for (const char* name : {"B", "G", "R"}) {
        put(channels, name, 2);
        put32(channels, 1);  // HALF
        channels.insert(channels.end(), {0, 0, 0, 0});  // pLinear, reserved
        put32(channels, 1);  // xSampling
        put32(channels, 1);  // ySampling
    }
    channels.push_back(0);
    attribute(out, "channels", "chlist", channels);
    attribute(out, "compression", "compression", {3});  // ZIP
    std::vector<std::uint8_t> box;
    put32(box, 0);
    put32(box, 0);
    put32(box, static_cast<std::uint32_t>(width - 1));
    put32(box, static_cast<std::uint32_t>(height - 1));
    attribute(out, "dataWindow", "box2i", box);
    attribute(out, "displayWindow", "box2i", box);
    attribute(out, "lineOrder", "lineOrder", {0});
    std::vector<std::uint8_t> one;
    put_float(one, 1.0f);
    attribute(out, "pixelAspectRatio", "float", one);
    std::vector<std::uint8_t> centre;
    put_float(centre, 0.0f);
    put_float(centre, 0.0f);
    attribute(out, "screenWindowCenter", "v2f", centre);
    attribute(out, "screenWindowWidth", "float", one);
    out.push_back(0);  // end of header

    const int blocks = (height + kLinesPerBlock - 1) / kLinesPerBlock;
    const std::size_t table_at = out.size();
    out.resize(out.size() + static_cast<std::size_t>(blocks) * 8);

    std::vector<std::uint8_t> raw, shuffled;
    const float* planes[3] = {&rgb[2 * n], &rgb[n], &rgb[0]};  // B, G, R
    for (int b = 0; b < blocks; ++b) {
        const int y0 = b * kLinesPerBlock, y1 = std::min(height, y0 + kLinesPerBlock);
        // Each line: every channel's halves in turn, little-endian.
        raw.clear();
        for (int y = y0; y < y1; ++y)
            for (const float* plane : planes)
                for (int x = 0; x < width; ++x) {
                    const std::uint16_t h = float_to_half(plane[static_cast<std::size_t>(y) * width + x]);
                    raw.push_back(static_cast<std::uint8_t>(h));
                    raw.push_back(static_cast<std::uint8_t>(h >> 8));
                }
        // OpenEXR's ZIP: bytes split into evens then odds, then each byte
        // stored as its difference from the one before.
        shuffled.resize(raw.size());
        const std::size_t half_size = (raw.size() + 1) / 2;
        for (std::size_t i = 0; i < raw.size(); ++i)
            shuffled[(i & 1) ? half_size + i / 2 : i / 2] = raw[i];
        for (std::size_t i = shuffled.size() - 1; i > 0; --i)
            shuffled[i] = static_cast<std::uint8_t>(static_cast<int>(shuffled[i]) - shuffled[i - 1] + 128 + 256);
        uLongf packed_size = compressBound(static_cast<uLong>(shuffled.size()));
        std::vector<std::uint8_t> packed(packed_size);
        const bool zipped = compress2(packed.data(), &packed_size, shuffled.data(),
                                      static_cast<uLong>(shuffled.size()), Z_DEFAULT_COMPRESSION) == Z_OK &&
                            packed_size < raw.size();

        const std::uint64_t at = out.size();
        for (int i = 0; i < 8; ++i) out[table_at + b * 8 + i] = static_cast<std::uint8_t>(at >> (8 * i));
        put32(out, static_cast<std::uint32_t>(y0));
        // Stored as is when deflate would not make it smaller; a reader
        // knows by the size.
        if (zipped) {
            put32(out, static_cast<std::uint32_t>(packed_size));
            put(out, packed.data(), packed_size);
        } else {
            put32(out, static_cast<std::uint32_t>(raw.size()));
            put(out, raw.data(), raw.size());
        }
    }

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool wrote = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    return std::fclose(f) == 0 && wrote;
}

}  // namespace ps::media

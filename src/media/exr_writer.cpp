#include "media/exr_writer.hpp"
#include "matting/parallel.hpp"

#include <zlib.h>

#include <cstdint>
#include <cstdio>
#include <algorithm>
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

    // Each block packed on its own, so on every core; then laid out in order.
    struct Block {
        std::vector<std::uint8_t> data;
        bool zipped = false;
    };
    std::vector<Block> packed_blocks(static_cast<std::size_t>(blocks));
    const float* planes[3] = {&rgb[2 * n], &rgb[n], &rgb[0]};  // B, G, R
    matting::parallel_for(static_cast<std::size_t>(blocks), [&](std::size_t b0, std::size_t b1) {
        std::vector<std::uint8_t> raw, shuffled;
        for (std::size_t b = b0; b < b1; ++b) {
            const int y0 = static_cast<int>(b) * kLinesPerBlock, y1 = std::min(height, y0 + kLinesPerBlock);
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
            Block& block = packed_blocks[b];
            block.data.resize(packed_size);
            // Level 4, OpenEXR's own default: most of the size of 6 for much
            // less time.
            block.zipped = compress2(block.data.data(), &packed_size, shuffled.data(),
                                     static_cast<uLong>(shuffled.size()), 4) == Z_OK &&
                           packed_size < raw.size();
            // Stored as is when deflate would not make it smaller; a reader
            // knows by the size.
            if (block.zipped) block.data.resize(packed_size);
            else block.data = raw;
        }
    }, 1);
    for (int b = 0; b < blocks; ++b) {
        const std::uint64_t at = out.size();
        for (int i = 0; i < 8; ++i) out[table_at + b * 8 + i] = static_cast<std::uint8_t>(at >> (8 * i));
        put32(out, static_cast<std::uint32_t>(b * kLinesPerBlock));
        put32(out, static_cast<std::uint32_t>(packed_blocks[b].data.size()));
        put(out, packed_blocks[b].data.data(), packed_blocks[b].data.size());
    }

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool wrote = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    return std::fclose(f) == 0 && wrote;
}

bool read_exr_half(const std::string& path, std::vector<float>* rgb, int* width, int* height) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<std::uint8_t> d;
    std::uint8_t chunk[1 << 16];
    for (std::size_t n; (n = std::fread(chunk, 1, sizeof chunk, f)) > 0;) d.insert(d.end(), chunk, chunk + n);
    std::fclose(f);
    if (d.size() < 8 || d[0] != 0x76 || d[1] != 0x2f || d[2] != 0x31 || d[3] != 0x01) return false;

    auto get32 = [&](std::size_t at) {
        return static_cast<std::uint32_t>(d[at]) | static_cast<std::uint32_t>(d[at + 1]) << 8 |
               static_cast<std::uint32_t>(d[at + 2]) << 16 | static_cast<std::uint32_t>(d[at + 3]) << 24;
    };
    // The header: name, type, size, value, until an empty name.
    std::size_t p = 8;
    int x0 = 0, y0 = 0, x1 = -1, y1 = -1, compression = -1;
    bool channels_ok = false;
    while (p < d.size() && d[p] != 0) {
        const std::size_t name_end = std::find(d.begin() + p, d.end(), 0) - d.begin();
        const std::string name(d.begin() + p, d.begin() + name_end);
        p = name_end + 1;
        const std::size_t type_end = std::find(d.begin() + p, d.end(), 0) - d.begin();
        p = type_end + 1;
        if (p + 4 > d.size()) return false;
        const std::uint32_t size = get32(p);
        p += 4;
        if (p + size > d.size()) return false;
        if (name == "dataWindow" && size == 16) {
            x0 = static_cast<int>(get32(p));
            y0 = static_cast<int>(get32(p + 4));
            x1 = static_cast<int>(get32(p + 8));
            y1 = static_cast<int>(get32(p + 12));
        } else if (name == "compression" && size == 1) {
            compression = d[p];
        } else if (name == "channels") {
            // B, G, R, all half: as written.
            const std::string expected[3] = {"B", "G", "R"};
            std::size_t q = p;
            int count = 0;
            channels_ok = true;
            while (q < p + size && d[q] != 0) {
                const std::size_t e = std::find(d.begin() + q, d.end(), 0) - d.begin();
                const std::string ch(d.begin() + q, d.begin() + e);
                if (count >= 3 || ch != expected[count] || get32(e + 1) != 1) channels_ok = false;
                q = e + 1 + 16;
                ++count;
            }
            channels_ok = channels_ok && count == 3;
        }
        p += size;
    }
    ++p;
    const int w = x1 - x0 + 1, h = y1 - y0 + 1;
    if (!channels_ok || w <= 0 || h <= 0 || (compression != 0 && compression != 3)) return false;
    const int lines_per_block = compression == 3 ? kLinesPerBlock : 1;
    const int blocks = (h + lines_per_block - 1) / lines_per_block;
    if (p + static_cast<std::size_t>(blocks) * 8 > d.size()) return false;

    const std::size_t n = static_cast<std::size_t>(w) * h;
    rgb->assign(n * 3, 0.0f);
    std::vector<std::uint8_t> raw, unpacked;
    for (int b = 0; b < blocks; ++b) {
        std::uint64_t at = 0;
        for (int k = 0; k < 8; ++k) at |= static_cast<std::uint64_t>(d[p + b * 8 + k]) << (8 * k);
        if (at + 8 > d.size()) return false;
        const int y = static_cast<int>(get32(static_cast<std::size_t>(at))) - y0;
        const std::uint32_t size = get32(static_cast<std::size_t>(at) + 4);
        if (at + 8 + size > d.size() || y < 0 || y >= h) return false;
        const int lines = std::min(lines_per_block, h - y);
        const std::size_t raw_size = static_cast<std::size_t>(lines) * 3 * w * 2;
        const std::uint8_t* data = &d[static_cast<std::size_t>(at) + 8];
        if (size < raw_size) {
            // Undo OpenEXR's ZIP: inflate, the running difference, the split.
            unpacked.resize(raw_size);
            uLongf got = static_cast<uLongf>(raw_size);
            if (uncompress(unpacked.data(), &got, data, size) != Z_OK || got != raw_size) return false;
            for (std::size_t i = 1; i < unpacked.size(); ++i)
                unpacked[i] = static_cast<std::uint8_t>(unpacked[i - 1] + unpacked[i] - 128);
            raw.resize(raw_size);
            const std::size_t half_size = (raw_size + 1) / 2;
            for (std::size_t i = 0; i < raw_size; ++i)
                raw[i] = unpacked[(i & 1) ? half_size + i / 2 : i / 2];
        } else {
            raw.assign(data, data + raw_size);
        }
        // Each line: B, then G, then R.
        for (int l = 0; l < lines; ++l)
            for (int c = 0; c < 3; ++c) {
                const int plane = 2 - c;  // B G R -> planes 2 1 0
                for (int x = 0; x < w; ++x) {
                    const std::size_t s = ((static_cast<std::size_t>(l) * 3 + c) * w + x) * 2;
                    const std::uint16_t hv = static_cast<std::uint16_t>(raw[s] | raw[s + 1] << 8);
                    (*rgb)[plane * n + static_cast<std::size_t>(y + l) * w + x] = half_to_float(hv);
                }
            }
    }
    *width = w;
    *height = h;
    return true;
}

}  // namespace ps::media

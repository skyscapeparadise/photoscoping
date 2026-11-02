#include "matting/png16.hpp"

#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

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

namespace {

// Any number of 16-bit channels, interleaved: greyscale (1) or RGBA (4).
bool write_samples(const std::string& path, const std::vector<std::uint16_t>& samples, int width,
                   int height, int channels, int level) {
    // Each row: filter type 1 (sub), which on a matte -- long runs of 0 and
    // of 65535 -- leaves mostly zeros for deflate to fold away. Two bytes a
    // sample, big-endian, as PNG has them.
    const std::size_t pixel = static_cast<std::size_t>(channels) * 2;
    const std::size_t row_bytes = 1 + static_cast<std::size_t>(width) * pixel;
    std::vector<std::uint8_t> raw(row_bytes * height);
    std::vector<std::uint8_t> row(static_cast<std::size_t>(width) * pixel);
    for (int y = 0; y < height; ++y) {
        const std::uint16_t* src = &samples[static_cast<std::size_t>(y) * width * channels];
        for (std::size_t i = 0; i < static_cast<std::size_t>(width) * channels; ++i) {
            row[i * 2] = static_cast<std::uint8_t>(src[i] >> 8);
            row[i * 2 + 1] = static_cast<std::uint8_t>(src[i]);
        }
        std::uint8_t* dst = &raw[y * row_bytes];
        dst[0] = 1;
        for (std::size_t i = 0; i < row.size(); ++i)
            dst[1 + i] = static_cast<std::uint8_t>(row[i] - (i >= pixel ? row[i - pixel] : 0));
    }

    uLongf packed_size = compressBound(static_cast<uLong>(raw.size()));
    std::vector<std::uint8_t> packed(packed_size);
    if (compress2(packed.data(), &packed_size, raw.data(), static_cast<uLong>(raw.size()), level) != Z_OK)
        return false;

    std::vector<std::uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<std::uint8_t> header;
    put32(header, static_cast<std::uint32_t>(width));
    put32(header, static_cast<std::uint32_t>(height));
    // 16-bit; greyscale or RGBA; then the defaults.
    header.insert(header.end(), {16, static_cast<std::uint8_t>(channels == 4 ? 6 : 0), 0, 0, 0});
    chunk(out, "IHDR", header.data(), header.size());
    chunk(out, "IDAT", packed.data(), packed_size);
    chunk(out, "IEND", nullptr, 0);

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool wrote = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    return std::fclose(f) == 0 && wrote;
}

std::uint32_t get32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

int paeth(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

}  // namespace

std::uint16_t to_sample(float v) {
    return static_cast<std::uint16_t>(std::clamp(v, 0.0f, 1.0f) * 65535.0f + 0.5f);
}

bool write_png16(const std::string& path, const std::vector<float>& values, int width, int height) {
    std::vector<std::uint16_t> samples(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) samples[i] = to_sample(values[i]);
    return write_samples(path, samples, width, height, 1, 6);
}

bool write_png16_rgba(const std::string& path, const std::vector<std::uint16_t>& rgba, int width,
                      int height) {
    // Deflate's fastest: a 4K frame is 66 MB of samples, and the default
    // takes five times as long for files a sixth smaller.
    return write_samples(path, rgba, width, height, 4, 1);
}

bool read_png16(const std::string& path, std::vector<float>* values, int* width, int* height) {
    int channels = 0;
    return read_png16_planes(path, values, width, height, &channels) && channels == 1;
}

bool read_png16_planes(const std::string& path, std::vector<float>* values, int* width, int* height,
                       int* channels_out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<std::uint8_t> file;
    std::uint8_t buffer[1 << 16];
    for (std::size_t n; (n = std::fread(buffer, 1, sizeof buffer, f)) > 0;)
        file.insert(file.end(), buffer, buffer + n);
    std::fclose(f);
    static const std::uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (file.size() < 8 || !std::equal(kSignature, kSignature + 8, file.begin())) return false;

    int w = 0, h = 0, depth = 0, type = -1, interlace = 0;
    std::vector<std::uint8_t> packed;
    for (std::size_t at = 8; at + 12 <= file.size();) {
        const std::uint32_t size = get32(&file[at]);
        if (at + 12 + size > file.size()) return false;
        const std::string kind(reinterpret_cast<const char*>(&file[at + 4]), 4);
        const std::uint8_t* data = &file[at + 8];
        if (kind == "IHDR" && size >= 13) {
            w = static_cast<int>(get32(data));
            h = static_cast<int>(get32(data + 4));
            depth = data[8];
            type = data[9];
            interlace = data[12];
        } else if (kind == "IDAT") {
            packed.insert(packed.end(), data, data + size);
        } else if (kind == "IEND") {
            break;
        }
        at += 12 + size;
    }
    // Greyscale, RGB or RGBA, 8 or 16 bits, not interlaced: what the stack
    // writes, and what anything else making a matte or a plate would.
    const int channels = type == 0 ? 1 : type == 2 ? 3 : type == 6 ? 4 : 0;
    if (!channels || (depth != 8 && depth != 16) || interlace != 0 || w <= 0 || h <= 0) return false;

    const int bpp = depth / 8 * channels;
    const std::size_t row_bytes = static_cast<std::size_t>(w) * bpp;
    std::vector<std::uint8_t> raw((row_bytes + 1) * h);
    uLongf raw_size = static_cast<uLongf>(raw.size());
    if (uncompress(raw.data(), &raw_size, packed.data(), static_cast<uLong>(packed.size())) != Z_OK ||
        raw_size != raw.size())
        return false;

    // Unfiltered in place, each row against the one above.
    std::vector<std::uint8_t> previous(row_bytes, 0);
    const std::size_t pixels = static_cast<std::size_t>(w) * h;
    values->resize(pixels * channels);
    for (int y = 0; y < h; ++y) {
        std::uint8_t* row = &raw[y * (row_bytes + 1) + 1];
        const int filter = row[-1];
        for (std::size_t i = 0; i < row_bytes; ++i) {
            const int a = i >= static_cast<std::size_t>(bpp) ? row[i - bpp] : 0;
            const int b = previous[i];
            const int c = i >= static_cast<std::size_t>(bpp) ? previous[i - bpp] : 0;
            int add = 0;
            switch (filter) {
                case 0: break;
                case 1: add = a; break;
                case 2: add = b; break;
                case 3: add = (a + b) / 2; break;
                case 4: add = paeth(a, b, c); break;
                default: return false;
            }
            row[i] = static_cast<std::uint8_t>(row[i] + add);
        }
        std::copy(row, row + row_bytes, previous.begin());
        // Planar out: every pixel's first channel, then its second.
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < channels; ++c) {
                const int i = x * channels + c;
                const float v = depth == 16 ? ((row[i * 2] << 8) | row[i * 2 + 1]) / 65535.0f
                                            : row[i] / 255.0f;
                (*values)[c * pixels + static_cast<std::size_t>(y) * w + x] = v;
            }
    }
    *width = w;
    *height = h;
    *channels_out = channels;
    return true;
}

}  // namespace ps::matting

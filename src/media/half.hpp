#pragma once

#include <cstdint>
#include <cstring>

// IEEE half floats, both ways, for what is written as half: EXR frames and
// textures. Round to nearest even; infinities and NaN kept; values past
// half's range saturate to its largest.
namespace ps::media {

inline float half_to_float(std::uint16_t h) {
    const std::uint32_t sign = (h & 0x8000u) << 16;
    const std::uint32_t exponent = (h >> 10) & 0x1Fu;
    const std::uint32_t mantissa = h & 0x3FFu;
    std::uint32_t bits;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            int e = -1;
            std::uint32_t m = mantissa;
            do {
                ++e;
                m <<= 1;
            } while ((m & 0x400u) == 0);
            bits = sign | static_cast<std::uint32_t>(127 - 15 - e) << 23 | (m & 0x3FFu) << 13;
        }
    } else if (exponent == 31) {
        bits = sign | 0x7F800000u | mantissa << 13;
    } else {
        bits = sign | (exponent + 127 - 15) << 23 | mantissa << 13;
    }
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

inline std::uint16_t float_to_half(float f) {
    std::uint32_t bits;
    std::memcpy(&bits, &f, sizeof bits);
    const std::uint16_t sign = static_cast<std::uint16_t>((bits >> 16) & 0x8000u);
    const std::uint32_t abs = bits & 0x7FFFFFFFu;
    if (abs >= 0x7F800000u) return sign | (abs > 0x7F800000u ? 0x7E00u : 0x7C00u);  // NaN, inf
    if (abs >= 0x477FF000u) return sign | 0x7BFFu;                                   // past 65504
    if (abs < 0x38800000u) {
        // Subnormal or zero: the mantissa, with its hidden bit, shifted down.
        if (abs < 0x33000000u) return sign;
        const std::uint32_t e = abs >> 23;
        const std::uint32_t m = (abs & 0x7FFFFFu) | 0x800000u;
        const std::uint32_t shift = 126 - e;
        std::uint32_t half = m >> (shift + 1);
        const std::uint32_t rest = m & ((1u << (shift + 1)) - 1);
        const std::uint32_t mid = 1u << shift;
        if (rest > mid || (rest == mid && (half & 1u))) ++half;
        return sign | static_cast<std::uint16_t>(half);
    }
    std::uint32_t half = ((abs >> 23) - 112) << 10 | ((abs >> 13) & 0x3FFu);
    const std::uint32_t rest = abs & 0x1FFFu;
    if (rest > 0x1000u || (rest == 0x1000u && (half & 1u))) ++half;
    return sign | static_cast<std::uint16_t>(half);
}

}  // namespace ps::media

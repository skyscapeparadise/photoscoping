#include "color/color_space.hpp"

#include <algorithm>
#include <cmath>

namespace ps::color {

namespace {

struct Chromaticity {
    float x, y;
};

struct Primaries {
    Chromaticity r, g, b, white;
};

constexpr Chromaticity kD65 = {0.3127f, 0.3290f};
constexpr Chromaticity kDciWhite = {0.3140f, 0.3510f};

Primaries primaries(Gamut gamut) {
    switch (gamut) {
        case Gamut::kRec709:
            return {{0.640f, 0.330f}, {0.300f, 0.600f}, {0.150f, 0.060f}, kD65};
        case Gamut::kRec2020:
            return {{0.708f, 0.292f}, {0.170f, 0.797f}, {0.131f, 0.046f}, kD65};
        case Gamut::kDciP3:
            return {{0.680f, 0.320f}, {0.265f, 0.690f}, {0.150f, 0.060f}, kDciWhite};
        case Gamut::kCinemaGamut:
            // Canon's published Cinema Gamut primaries. Green and blue sit
            // outside the spectral locus, which is legal for an encoding gamut.
            return {{0.740f, 0.270f}, {0.170f, 1.140f}, {0.080f, -0.100f}, kD65};
    }
    return primaries(Gamut::kRec709);
}

std::array<float, 3> xyz(Chromaticity c) {
    return {c.x / c.y, 1.0f, (1.0f - c.x - c.y) / c.y};
}

Mat3 inverse(const Mat3& m) {
    const float a = m[0], b = m[1], c = m[2];
    const float d = m[3], e = m[4], f = m[5];
    const float g = m[6], h = m[7], i = m[8];
    const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const float det = a * A + b * B + c * C;
    const float s = 1.0f / det;
    return {A * s, -(b * i - c * h) * s, (b * f - c * e) * s,
            B * s, (a * i - c * g) * s, -(a * f - c * d) * s,
            C * s, -(a * h - b * g) * s, (a * e - b * d) * s};
}

// RGB -> XYZ for a set of primaries (SMPTE RP 177).
Mat3 rgb_to_xyz(const Primaries& p) {
    const auto r = xyz(p.r), g = xyz(p.g), b = xyz(p.b), w = xyz(p.white);
    const Mat3 m = {r[0], g[0], b[0], r[1], g[1], b[1], r[2], g[2], b[2]};
    const auto s = apply_matrix(inverse(m), w);
    return {m[0] * s[0], m[1] * s[1], m[2] * s[2],
            m[3] * s[0], m[4] * s[1], m[5] * s[2],
            m[6] * s[0], m[7] * s[1], m[8] * s[2]};
}

// Bradford chromatic adaptation between two white points.
Mat3 bradford(Chromaticity from, Chromaticity to) {
    const Mat3 cone = {0.8951f, 0.2664f, -0.1614f,
                       -0.7502f, 1.7135f, 0.0367f,
                       0.0389f, -0.0685f, 1.0296f};
    const auto src = apply_matrix(cone, xyz(from));
    const auto dst = apply_matrix(cone, xyz(to));
    const Mat3 scale = {dst[0] / src[0], 0, 0, 0, dst[1] / src[1], 0, 0, 0, dst[2] / src[2]};
    return multiply(inverse(cone), multiply(scale, cone));
}

Mat3 convert(Gamut from, Gamut to) {
    const Primaries src = primaries(from), dst = primaries(to);
    Mat3 m = rgb_to_xyz(src);
    if (src.white.x != dst.white.x || src.white.y != dst.white.y)
        m = multiply(bradford(src.white, dst.white), m);
    return multiply(inverse(rgb_to_xyz(dst)), m);
}

// SMPTE ST 2084 constants.
constexpr float kPqM1 = 2610.0f / 16384.0f;
constexpr float kPqM2 = 2523.0f / 4096.0f * 128.0f;
constexpr float kPqC1 = 3424.0f / 4096.0f;
constexpr float kPqC2 = 2413.0f / 4096.0f * 32.0f;
constexpr float kPqC3 = 2392.0f / 4096.0f * 32.0f;

// BT.2100 HLG constants and the reference display the OOTF assumes.
constexpr float kHlgA = 0.17883277f;
constexpr float kHlgB = 0.28466892f;
constexpr float kHlgC = 0.55991073f;
constexpr float kHlgPeakNits = 1000.0f;
constexpr float kHlgSystemGamma = 1.2f;

float hlg_inverse_oetf(float v) {
    return v <= 0.5f ? v * v / 3.0f : (std::exp((v - kHlgC) / kHlgA) + kHlgB) / 12.0f;
}

}  // namespace

const char* gamut_name(Gamut gamut) {
    switch (gamut) {
        case Gamut::kRec709:      return "Rec.709";
        case Gamut::kRec2020:     return "Rec.2020";
        case Gamut::kDciP3:       return "DCI-P3";
        case Gamut::kCinemaGamut: return "Cinema Gamut";
    }
    return "?";
}

const char* transfer_name(Transfer transfer) {
    switch (transfer) {
        case Transfer::kBt1886:  return "BT.1886";
        case Transfer::kPq:      return "PQ";
        case Transfer::kHlg:     return "HLG";
        case Transfer::kGamma26: return "gamma 2.6";
        case Transfer::kLinear:  return "linear";
    }
    return "?";
}

Mat3 multiply(const Mat3& a, const Mat3& b) {
    Mat3 out{};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            out[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
    return out;
}

std::array<float, 3> apply_matrix(const Mat3& m, const std::array<float, 3>& v) {
    return {m[0] * v[0] + m[1] * v[1] + m[2] * v[2],
            m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
            m[6] * v[0] + m[7] * v[1] + m[8] * v[2]};
}

Mat3 to_working(Gamut gamut) { return convert(gamut, Gamut::kRec2020); }

Mat3 working_to_rec709() { return convert(Gamut::kRec2020, Gamut::kRec709); }

float decode_channel(Transfer transfer, float signal) {
    switch (transfer) {
        case Transfer::kBt1886:
            return std::pow(std::max(signal, 0.0f), 2.4f);
        case Transfer::kGamma26:
            return std::pow(std::max(signal, 0.0f), 2.6f);
        case Transfer::kLinear:
            return signal;
        case Transfer::kPq: {
            const float p = std::pow(std::max(signal, 0.0f), 1.0f / kPqM2);
            const float nits =
                10000.0f * std::pow(std::max(p - kPqC1, 0.0f) / (kPqC2 - kPqC3 * p), 1.0f / kPqM1);
            return nits / kReferenceWhiteNits;
        }
        case Transfer::kHlg: {
            // Achromatic shortcut: for R = G = B the OOTF's luminance is the
            // channel itself. hlg_to_working() is the general case.
            const float e = hlg_inverse_oetf(std::max(signal, 0.0f));
            return kHlgPeakNits * std::pow(e, kHlgSystemGamma) / kReferenceWhiteNits;
        }
    }
    return signal;
}

std::array<float, 3> hlg_to_working(const std::array<float, 3>& signal) {
    const std::array<float, 3> e = {hlg_inverse_oetf(std::max(signal[0], 0.0f)),
                                    hlg_inverse_oetf(std::max(signal[1], 0.0f)),
                                    hlg_inverse_oetf(std::max(signal[2], 0.0f))};
    // The OOTF scales by scene luminance, computed with Rec.2020 weights since
    // HLG is defined in Rec.2020.
    const float ys = 0.2627f * e[0] + 0.6780f * e[1] + 0.0593f * e[2];
    const float gain = ys > 0.0f ? kHlgPeakNits * std::pow(ys, kHlgSystemGamma - 1.0f) : 0.0f;
    return {gain * e[0] / kReferenceWhiteNits, gain * e[1] / kReferenceWhiteNits,
            gain * e[2] / kReferenceWhiteNits};
}

}  // namespace ps::color

#pragma once

#include <array>

namespace ps::color {

// The working space every clip is converted into after its input transform:
// linear light, Rec.2020 primaries, D65, with 1.0 meaning SDR reference white.
// HDR sources land above 1.0 (PQ and HLG reference white is 203 nits, BT.2408),
// so SDR and HDR clips share one scale and every correction downstream behaves
// the same on both.
inline constexpr float kReferenceWhiteNits = 203.0f;

// RGB primaries a signal can be encoded in.
enum class Gamut {
    kRec709,
    kRec2020,
    // DCI-P3 with the DCI white point (not D65), as Canon's DCIP3_DCI LUTs
    // output. Converting it to the D65 working space needs a white-point
    // adaptation, not just a primaries change.
    kDciP3,
    kCinemaGamut,
};

// How a signal is encoded, i.e. what has to be undone to get linear light.
enum class Transfer {
    kBt1886,    // SDR video: gamma 2.4 display EOTF (Rec.709 and Wide DR)
    kPq,        // SMPTE ST 2084
    kHlg,       // ARIB STD-B67 / BT.2100 HLG, through the 1000-nit reference OOTF
    kGamma26,   // DCI cinema
    kLinear,
};

const char* gamut_name(Gamut gamut);
const char* transfer_name(Transfer transfer);

// Row-major 3x3.
using Mat3 = std::array<float, 9>;

Mat3 multiply(const Mat3& a, const Mat3& b);
std::array<float, 3> apply_matrix(const Mat3& m, const std::array<float, 3>& v);

// Linear RGB in `gamut` to linear Rec.2020 D65 (the working space). Built from
// the published primaries rather than hardcoded, so every gamut goes through
// the same arithmetic, which the tests check against BT.2087's matrix.
Mat3 to_working(Gamut gamut);

// Linear Rec.2020 to linear Rec.709/sRGB primaries, for the display. Values
// outside 709 come out negative or above 1, which the extended-linear
// swapchain shows as-is and the SDR one clamps.
Mat3 working_to_rec709();

// One channel of encoded signal (0..1) to linear light relative to reference
// white. CPU mirrors of the shader functions in src/gpu/shaders/common.glsl,
// kept here so the curves can be tested without a GPU. HLG is not here because
// its OOTF depends on all three channels; see hlg_to_working().
float decode_channel(Transfer transfer, float signal);
std::array<float, 3> hlg_to_working(const std::array<float, 3>& signal);

}  // namespace ps::color

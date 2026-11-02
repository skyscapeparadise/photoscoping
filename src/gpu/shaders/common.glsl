// Shared colour functions. src/color/color_space.cpp holds CPU mirrors of
// these, which is where they are tested; keep the two in step.

// Must match ps::color::Transfer.
const int kTransferBt1886 = 0;
const int kTransferPq = 1;
const int kTransferHlg = 2;
const int kTransferGamma26 = 3;
const int kTransferLinear = 4;
const int kTransferSrgb = 5;
const int kTransferCanonLog = 6;
const int kTransferCanonLog2 = 7;
const int kTransferCanonLog3 = 8;

const float kReferenceWhiteNits = 203.0;

vec3 pq_to_relative(vec3 signal) {
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 4096.0 * 128.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 4096.0 * 32.0;
    const float c3 = 2392.0 / 4096.0 * 32.0;
    vec3 p = pow(max(signal, vec3(0.0)), vec3(1.0 / m2));
    vec3 nits = 10000.0 * pow(max(p - c1, vec3(0.0)) / (c2 - c3 * p), vec3(1.0 / m1));
    return nits / kReferenceWhiteNits;
}

// BT.2100 HLG through the reference OOTF of a 1000-nit display. The OOTF
// scales by scene luminance, so it acts on all three channels together.
vec3 hlg_to_relative(vec3 signal) {
    const float a = 0.17883277;
    const float b = 0.28466892;
    const float c = 0.55991073;
    vec3 v = max(signal, vec3(0.0));
    vec3 low = v * v / 3.0;
    vec3 high = (exp((v - c) / a) + b) / 12.0;
    vec3 e = mix(low, high, step(vec3(0.5), v));
    float ys = dot(e, vec3(0.2627, 0.6780, 0.0593));
    float gain = ys > 0.0 ? 1000.0 * pow(ys, 0.2) : 0.0;
    return gain * e / kReferenceWhiteNits;
}

// sRGB encoded -> linear. Interface colours are authored encoded.
vec3 srgb_eotf(vec3 c) {
    vec3 lo = c / 12.92;
    vec3 hi = pow((c + 0.055) / 1.055, vec3(2.4));
    return mix(hi, lo, lessThanEqual(c, vec3(0.04045)));
}

float srgb_eotf(float c) { return srgb_eotf(vec3(c)).x; }

// linear -> sRGB encoded, clamped. Exact inverse of srgb_eotf.
vec3 srgb_oetf(vec3 linear) {
    vec3 l = clamp(linear, 0.0, 1.0);
    return mix(l * 12.92, 1.055 * pow(l, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), l));
}

// Canon's logs to scene light (color_space.hpp has the why of each).
vec3 canon_log3(vec3 x) {
    vec3 below = -(pow(vec3(10.0), (0.12783901 - x) / 0.36726845) - 1.0) / 14.98325;
    vec3 toe = (x - 0.12512219) / 1.9754798;
    vec3 above = (pow(vec3(10.0), (x - 0.12240537) / 0.36726845) - 1.0) / 14.98325;
    return mix(mix(below, toe, step(vec3(0.097465473), x)), above, step(vec3(0.15277891) + 1e-7, x));
}
vec3 canon_log2(vec3 signal) {
    vec3 x = (signal * 1023.0 - 64.0) / 876.0;
    vec3 below = -(pow(vec3(10.0), (0.035388128 - x) / 0.281864443) - 1.0) / 87.09937546;
    vec3 above = (pow(vec3(10.0), (x - 0.035388128) / 0.281864443) - 1.0) / 87.09937546;
    return mix(below, above, step(vec3(0.092864125), x));
}
vec3 canon_log(vec3 signal) {
    vec3 x = (signal * 1023.0 - 64.0) / 876.0;
    vec3 below = -(pow(vec3(10.0), (0.0730597 - x) / 0.529136) - 1.0) / 10.1596;
    vec3 above = (pow(vec3(10.0), (x - 0.0730597) / 0.529136) - 1.0) / 10.1596;
    return mix(below, above, step(vec3(0.0730597), x));
}

// Encoded signal -> linear light, 1.0 = reference white.
vec3 decode_transfer(vec3 signal, int transfer) {
    if (transfer == kTransferCanonLog3) return canon_log3(signal);
    if (transfer == kTransferCanonLog2) return canon_log2(signal);
    if (transfer == kTransferCanonLog) return canon_log(signal);
    if (transfer == kTransferPq) return pq_to_relative(signal);
    if (transfer == kTransferHlg) return hlg_to_relative(signal);
    if (transfer == kTransferGamma26) return pow(max(signal, vec3(0.0)), vec3(2.6));
    if (transfer == kTransferLinear) return signal;
    if (transfer == kTransferSrgb) return srgb_eotf(max(signal, vec3(0.0)));
    return pow(max(signal, vec3(0.0)), vec3(2.4));
}

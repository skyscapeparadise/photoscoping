// Shared colour functions. src/color/color_space.cpp holds CPU mirrors of
// these, which is where they are tested; keep the two in step.

// Must match ps::color::Transfer.
const int kTransferBt1886 = 0;
const int kTransferPq = 1;
const int kTransferHlg = 2;
const int kTransferGamma26 = 3;
const int kTransferLinear = 4;

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

// Encoded signal -> linear light, 1.0 = reference white.
vec3 decode_transfer(vec3 signal, int transfer) {
    if (transfer == kTransferPq) return pq_to_relative(signal);
    if (transfer == kTransferHlg) return hlg_to_relative(signal);
    if (transfer == kTransferGamma26) return pow(max(signal, vec3(0.0)), vec3(2.6));
    if (transfer == kTransferLinear) return signal;
    return pow(max(signal, vec3(0.0)), vec3(2.4));
}

vec3 srgb_encode(vec3 linear) {
    vec3 l = clamp(linear, 0.0, 1.0);
    return mix(l * 12.92, 1.055 * pow(l, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), l));
}

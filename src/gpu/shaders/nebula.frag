#version 450

#include "common.glsl"

// The inspector panel's backdrop: dark purple falling to black, with the
// structure of a nebula -- glowing cloud, darker dust lanes through it, and a
// few faint stars -- rather than a plain gradient.
//
// The cloud is fractal noise fed back into itself (domain warping): the
// coordinates are pushed around by one noise field before sampling the next,
// which turns blobs into the folded, filamentary shapes of real gas.
//
// Everything is measured in logical pixels from the panel's top-right corner.
// Widening the panel uncovers more of the same sky to the left rather than
// stretching what is already there, and sliding it away carries the sky with
// it.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 3, binding = 0) uniform Nebula {
    vec2 size;           // the part on screen, logical pixels
    float panel_width;   // the whole panel, of which that is the left part
    float sdr_ref_nits;
} nebula;

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float value_noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float a = hash12(i);
    float b = hash12(i + vec2(1.0, 0.0));
    float c = hash12(i + vec2(0.0, 1.0));
    float d = hash12(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// Rotating between octaves keeps the lattice from lining up into visible
// horizontal and vertical streaks.
const mat2 kOctave = mat2(0.80, 0.60, -0.60, 0.80);

float fbm(vec2 p) {
    float sum = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 6; ++i) {
        sum += amp * value_noise(p);
        p = kOctave * p * 2.03 + vec2(17.1, 9.2);
        amp *= 0.5;
    }
    return sum / 0.984;
}

// Sparse single-pixel-ish stars: at most one per cell, most cells empty.
float stars(vec2 px, float cell, float density) {
    vec2 c = floor(px / cell);
    float h = hash12(c + 71.3);
    if (h > density) return 0.0;
    vec2 at = (vec2(hash12(c + 3.1), hash12(c + 9.7)) * 0.8 + 0.1) * cell;
    float d = length(px - c * cell - at);
    float twinkle = 0.4 + 0.6 * hash12(c + 23.9);
    return twinkle * exp(-d * d * 1.6);
}

void main() {
    vec2 px = vec2(v_uv.x * nebula.size.x - nebula.panel_width, v_uv.y * nebula.size.y);
    vec2 p = px / 300.0 + vec2(3.7, 1.9);

    vec2 q = vec2(fbm(p), fbm(p + vec2(5.2, 1.3)));
    vec2 r = vec2(fbm(p + 3.2 * q + vec2(1.7, 9.2)), fbm(p + 3.2 * q + vec2(8.3, 2.8)));
    float f = fbm(p + 3.0 * r);

    // The large-scale shape: light gathers in the upper part of the panel and
    // falls away to black toward the bottom. Measured against the panel's
    // height so the fall-off keeps its proportions as the window resizes.
    vec2 n = px / max(nebula.size.y, 1.0);
    float fall = 1.0 - smoothstep(0.05, 1.05, n.y);
    vec2 g = (n - vec2(-0.30, 0.28)) * vec2(1.5, 2.1);
    float glow = exp(-dot(g, g));

    const vec3 kVoid   = vec3(0.012, 0.006, 0.020);
    const vec3 kDeep   = vec3(0.110, 0.035, 0.170);
    const vec3 kPlum   = vec3(0.300, 0.075, 0.380);
    const vec3 kOrchid = vec3(0.560, 0.200, 0.640);
    const vec3 kVeil   = vec3(0.820, 0.560, 0.900);

    vec3 col = kVoid;
    col = mix(col, kDeep, clamp(f * f * 2.2, 0.0, 1.0) * (0.35 + 0.65 * fall));
    col = mix(col, kPlum, clamp(length(q) * 0.9 - 0.35, 0.0, 1.0) * fall * (0.4 + 0.6 * glow));
    col = mix(col, kOrchid, smoothstep(0.55, 0.95, f) * glow * fall);
    col += kVeil * pow(smoothstep(0.62, 1.0, f * (0.6 + 0.6 * r.x)), 3.0) * glow * 0.45;

    // Dust lanes: dark filaments where the warp field folds over itself.
    float lane = smoothstep(0.30, 0.62, r.y) * 0.65 + 0.35;
    col *= mix(1.0, lane, 0.85 * fall);

    // A few stars, dimmer where the cloud is thick, as if seen through it.
    float s = stars(px, 46.0, 0.10) + 0.6 * stars(px + 400.0, 23.0, 0.05);
    col += vec3(0.85, 0.80, 1.0) * s * (1.0 - smoothstep(0.35, 0.8, f)) * 0.55;

    // Dither in encoded space: a dark gradient through an 8-bit swapchain
    // bands visibly without it.
    float dither = (hash12(gl_FragCoord.xy) + hash12(gl_FragCoord.yx + 7.0) - 1.0) / 255.0;
    col = clamp(col + dither, 0.0, 1.0);

    out_color = vec4(srgb_eotf(col) * nebula.sdr_ref_nits, 1.0);
}

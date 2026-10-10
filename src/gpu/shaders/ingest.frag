#version 450

#include "common.glsl"

// Decoded planes -> the working space: linear light, Rec.2020, 1.0 = white.
//
// The order is the whole point: YCbCr -> full-range R'G'B' -> input LUT ->
// linearise -> working primaries. A camera-log LUT has to see the signal
// exactly as recorded, so nothing happens between the YCbCr decode and the
// LUT.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D y_plane;
// Cb, or interleaved CbCr when ingest.interleaved is set.
layout(set = 2, binding = 1) uniform sampler2D cb_plane;
layout(set = 2, binding = 2) uniform sampler2D cr_plane;
layout(set = 2, binding = 3) uniform sampler3D lut;

layout(set = 3, binding = 0) uniform Ingest {
    // YCbCr -> R'G'B', range expansion included (ps::color::ycbcr_to_rgb).
    vec4 ycbcr_r;
    vec4 ycbcr_g;
    vec4 ycbcr_b;
    // Linear RGB in the signal's primaries -> Rec.2020. xyz used.
    vec4 to_working_r;
    vec4 to_working_g;
    vec4 to_working_b;
    vec4 lut_domain_min;  // w: LUT size, 0 when there is no LUT
    vec4 lut_domain_max;
    float sample_scale;
    float interleaved;
    float transfer;       // ps::color::Transfer of the signal after the LUT
    float pad;
} ingest;

vec3 lut_at(ivec3 p) { return texelFetch(lut, p, 0).rgb; }

// Tetrahedral interpolation, mirroring ps::color::CubeLut::sample. Done by
// hand with texelFetch rather than hardware trilinear filtering, which is both
// less accurate on the neutral axis and limited to 8 bits of fraction on some
// GPUs.
vec3 apply_lut(vec3 rgb) {
    float size = ingest.lut_domain_min.w;
    vec3 t = clamp((rgb - ingest.lut_domain_min.xyz) /
                   (ingest.lut_domain_max.xyz - ingest.lut_domain_min.xyz), 0.0, 1.0);
    vec3 p = t * (size - 1.0);
    vec3 base = min(floor(p), vec3(size - 2.0));
    vec3 f = p - base;
    ivec3 i = ivec3(base);

    vec3 c0 = lut_at(i);
    vec3 c3 = lut_at(i + ivec3(1, 1, 1));
    vec3 c1, c2;
    vec4 w;
    if (f.r > f.g) {
        if (f.g > f.b) {
            c1 = lut_at(i + ivec3(1, 0, 0)); c2 = lut_at(i + ivec3(1, 1, 0));
            w = vec4(1.0 - f.r, f.r - f.g, f.g - f.b, f.b);
        } else if (f.r > f.b) {
            c1 = lut_at(i + ivec3(1, 0, 0)); c2 = lut_at(i + ivec3(1, 0, 1));
            w = vec4(1.0 - f.r, f.r - f.b, f.b - f.g, f.g);
        } else {
            c1 = lut_at(i + ivec3(0, 0, 1)); c2 = lut_at(i + ivec3(1, 0, 1));
            w = vec4(1.0 - f.b, f.b - f.r, f.r - f.g, f.g);
        }
    } else {
        if (f.b > f.g) {
            c1 = lut_at(i + ivec3(0, 0, 1)); c2 = lut_at(i + ivec3(0, 1, 1));
            w = vec4(1.0 - f.b, f.b - f.g, f.g - f.r, f.r);
        } else if (f.b > f.r) {
            c1 = lut_at(i + ivec3(0, 1, 0)); c2 = lut_at(i + ivec3(0, 1, 1));
            w = vec4(1.0 - f.g, f.g - f.b, f.b - f.r, f.r);
        } else {
            c1 = lut_at(i + ivec3(0, 1, 0)); c2 = lut_at(i + ivec3(1, 1, 0));
            w = vec4(1.0 - f.g, f.g - f.r, f.r - f.b, f.b);
        }
    }
    return w.x * c0 + w.y * c1 + w.z * c2 + w.w * c3;
}

void main() {
    float y = texture(y_plane, v_uv).r;
    vec2 cbcr = ingest.interleaved > 0.5
                    ? texture(cb_plane, v_uv).rg
                    : vec2(texture(cb_plane, v_uv).r, texture(cr_plane, v_uv).r);
    vec4 ycc = vec4(vec3(y, cbcr) * ingest.sample_scale, 1.0);
    vec3 rgb = vec3(dot(ingest.ycbcr_r, ycc), dot(ingest.ycbcr_g, ycc), dot(ingest.ycbcr_b, ycc));

    if (ingest.lut_domain_min.w > 0.0) rgb = apply_lut(rgb);

    vec3 linear = decode_transfer(rgb, int(ingest.transfer + 0.5));
    vec3 working = vec3(dot(ingest.to_working_r.xyz, linear),
                        dot(ingest.to_working_g.xyz, linear),
                        dot(ingest.to_working_b.xyz, linear));
    out_color = vec4(working, 1.0);
}

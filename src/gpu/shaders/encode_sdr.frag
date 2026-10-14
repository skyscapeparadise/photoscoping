#version 450

#include "common.glsl"

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D linear_target;

layout(set = 3, binding = 0) uniform Encode {
    float sdr_ref_nits;
    float pad0;
    float pad1;
    float pad2;
} encode;

// SDR: normalise by the same reference white the UI shader wrote against, then
// sRGB-encode. Exact inverse of that write path, so a UI colour round-trips to
// the byte it was authored as.
void main() {
    vec3 nits = texture(linear_target, v_uv).rgb;
    vec3 normalized = clamp(nits / encode.sdr_ref_nits, 0.0, 1.0);
    out_color = vec4(srgb_oetf(normalized), 1.0);
}

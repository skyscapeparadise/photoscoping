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

// Extended linear, which is how macOS composits: 1.0 is SDR white -- whatever
// the brightness slider currently means -- and anything above it uses the
// display's headroom.
//
// So this divides by the same reference white the UI wrote against and stops
// there. No sRGB curve, because the swapchain is linear, and no clamp, because
// values above 1.0 are the entire point: they are the highlights.
//
// The difference from PQ is what the number means. In ST2084 a UI white of 203
// nits is an absolute instruction to the display, ignoring what the user set
// their screen to; here the same pixel becomes 1.0 and lands wherever their SDR
// white is, alongside every other application.
void main() {
    vec3 nits = texture(linear_target, v_uv).rgb;
    out_color = vec4(max(nits / encode.sdr_ref_nits, vec3(0.0)), 1.0);
}

#version 450

#include "common.glsl"

// The working image (linear Rec.2020, 1.0 = reference white) -> the window.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D working;

layout(set = 3, binding = 0) uniform Display {
    // Rec.2020 -> Rec.709/sRGB primaries. xyz used.
    vec4 to_display_r;
    vec4 to_display_g;
    vec4 to_display_b;
    // 1 for the extended-linear (EDR) swapchain, which takes linear values
    // as-is with 1.0 at the user's SDR white and headroom above it. 0 for the
    // plain SDR swapchain, which needs clamping and an sRGB encode.
    float extended;
    float pad0, pad1, pad2;
} display;

void main() {
    vec3 c = texture(working, v_uv).rgb;
    vec3 rgb = vec3(dot(display.to_display_r.xyz, c), dot(display.to_display_g.xyz, c),
                    dot(display.to_display_b.xyz, c));
    out_color = vec4(display.extended > 0.5 ? rgb : srgb_encode(rgb), 1.0);
}

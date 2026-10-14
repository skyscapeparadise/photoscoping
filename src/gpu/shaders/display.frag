#version 450

// The working image (linear Rec.2020, 1.0 = reference white) -> the window's
// linear target (linear Rec.709 nits), which the final encode turns into
// whatever the swapchain wants.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D working;

layout(set = 3, binding = 0) uniform Display {
    // Rec.2020 -> Rec.709/sRGB primaries. xyz used.
    vec4 to_display_r;
    vec4 to_display_g;
    vec4 to_display_b;
    // Nits at a working value of 1.0: the reference white the interface
    // writes against, so footage white and interface white are the same.
    float nits_per_unit;
    float pad0, pad1, pad2;
} display;

void main() {
    vec3 c = texture(working, v_uv).rgb;
    vec3 rgb = vec3(dot(display.to_display_r.xyz, c), dot(display.to_display_g.xyz, c),
                    dot(display.to_display_b.xyz, c));
    out_color = vec4(rgb * display.nits_per_unit, 1.0);
}

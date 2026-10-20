#version 450

#include "common.glsl"

// The working image (linear Rec.2020, 1.0 = reference white) -> the window's
// linear target (linear Rec.709 nits), which the final encode turns into
// whatever the swapchain wants.
//
// Shown one of four ways. The renders it reads are the clip's own frame,
// matted: `matte` holds coverage in its red channel, `subject` the refined
// subject already multiplied by it, over black. `garbage` is what the hand-
// drawn garbage mattes leave of the frame, multiplied into both.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D working;
layout(set = 2, binding = 1) uniform sampler2D matte;
layout(set = 2, binding = 2) uniform sampler2D subject;
layout(set = 2, binding = 3) uniform sampler2D garbage;

// Must match ps::gpu::DisplayView.
const int kViewFootage = 0;
const int kViewMatte = 1;
const int kViewSubject = 2;
const int kViewOverlay = 3;

layout(set = 3, binding = 0) uniform Display {
    // Rec.2020 -> Rec.709/sRGB primaries. xyz used.
    vec4 to_display_r;
    vec4 to_display_g;
    vec4 to_display_b;
    // Overlay wash, linear Rec.709, and how strongly it covers. xyz, w.
    vec4 wash;
    // Nits at a working value of 1.0: the reference white the interface
    // writes against, so footage white and interface white are the same.
    float nits_per_unit;
    float view;
    // The checkerboard behind a subject: one square, in target pixels.
    float checker_px;
    // 1 when there are garbage mattes to apply.
    float has_garbage;
} display;

vec3 to_display(vec3 c) {
    return vec3(dot(display.to_display_r.xyz, c), dot(display.to_display_g.xyz, c),
                dot(display.to_display_b.xyz, c));
}

void main() {
    int view = int(display.view + 0.5);
    vec3 rgb;
    if (view == kViewFootage) {
        rgb = to_display(texture(working, v_uv).rgb);
    } else {
        float g = display.has_garbage > 0.5 ? texture(garbage, v_uv).r : 1.0;
        float a = clamp(texture(matte, v_uv).r, 0.0, 1.0) * g;
        if (view == kViewMatte) {
            // As a matte reads in any player: its values shown as encoded
            // greys, so 0.5 coverage is a middle grey rather than a dark one.
            rgb = srgb_eotf(vec3(a));
        } else if (view == kViewSubject) {
            vec2 cell = floor(gl_FragCoord.xy / display.checker_px);
            float light = mod(cell.x + cell.y, 2.0);
            vec3 behind = srgb_eotf(vec3(mix(0.16, 0.24, light)));
            rgb = to_display(texture(subject, v_uv).rgb) * g + (1.0 - a) * behind;
        } else {
            // What the matte keeps, untouched; what it drops, washed over --
            // so an edge reads against the footage it was cut from.
            vec3 footage = to_display(texture(working, v_uv).rgb);
            vec3 washed = mix(footage, display.wash.rgb, display.wash.w);
            rgb = mix(washed, footage, a);
        }
    }
    out_color = vec4(rgb * display.nits_per_unit, 1.0);
}

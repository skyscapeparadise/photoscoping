#version 450

#include "common.glsl"

// The working image (linear Rec.2020, 1.0 = reference white) -> the window's
// linear target (linear Rec.709 nits), which the final encode turns into
// whatever the swapchain wants.
//
// Shown one of four ways. The renders it reads are the clip's own frame,
// matted: `matte` holds coverage in its red channel, `subject` the refined
// subject already multiplied by it, over black. `fixes` is what the hand-made
// fixes do to the matte (fixes/mask.hpp): red, what the garbage mattes leave;
// green and blue, the paint strokes folded into alpha -> green * alpha + blue.
// `rematte` is the re-matte patches on this frame (fixes/rematte.hpp), over
// the rectangle `rematte_rect` of the image: red the closer look's alpha,
// green how much of it to take. They replace the matte before the fixes
// apply, so a garbage matte or a stroke still has the last word.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D working;
layout(set = 2, binding = 1) uniform sampler2D matte;
layout(set = 2, binding = 2) uniform sampler2D subject;
layout(set = 2, binding = 3) uniform sampler2D fixes;
layout(set = 2, binding = 4) uniform sampler2D rematte;

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
    // 1 when there are fixes to apply.
    float has_fixes;
    // The patches' rectangle in the image's UV: x0, y0, x1, y1.
    vec4 rematte_rect;
    // 1 when there are patches on this frame.
    float has_rematte;
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
        vec4 f = display.has_fixes > 0.5 ? texture(fixes, v_uv) : vec4(1.0, 1.0, 0.0, 1.0);
        float m = clamp(texture(matte, v_uv).r, 0.0, 1.0);
        vec4 r = display.rematte_rect;
        if (display.has_rematte > 0.5 && all(greaterThanEqual(v_uv, r.xy)) &&
            all(lessThan(v_uv, r.zw))) {
            vec2 p = texture(rematte, (v_uv - r.xy) / (r.zw - r.xy)).rg;
            m = mix(m, p.r, p.g);
        }
        float a = (m * f.g + f.b) * f.r;
        if (view == kViewMatte) {
            // As a matte reads in any player: its values shown as encoded
            // greys, so 0.5 coverage is a middle grey rather than a dark one.
            rgb = srgb_eotf(vec3(a));
        } else if (view == kViewSubject) {
            vec2 cell = floor(gl_FragCoord.xy / display.checker_px);
            float light = mod(cell.x + cell.y, 2.0);
            vec3 behind = srgb_eotf(vec3(mix(0.16, 0.24, light)));
            // Coverage painted in has no subject colour from the stack -- it
            // called that background -- so it takes the footage's.
            vec3 painted = to_display(texture(subject, v_uv).rgb) * f.g +
                           to_display(texture(working, v_uv).rgb) * f.b;
            rgb = painted * f.r + (1.0 - a) * behind;
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

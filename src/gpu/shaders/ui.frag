#version 450

#include "common.glsl"

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_fill;
layout(location = 2) in vec4 v_border;
layout(location = 3) in vec4 v_rect;
layout(location = 4) in vec4 v_params;
layout(location = 5) in vec4 v_misc;
layout(location = 6) in vec2 v_pixel;

layout(location = 0) out vec4 out_color;

// Four texture slots, selected per-vertex. Batching can only merge consecutive
// primitives, so with a single binding a panel that interleaves solid rects,
// icons and glyphs fragments into a draw call per run.
layout(set = 2, binding = 0) uniform sampler2D tex0;
layout(set = 2, binding = 1) uniform sampler2D tex1;
layout(set = 2, binding = 2) uniform sampler2D tex2;
layout(set = 2, binding = 3) uniform sampler2D tex3;

layout(set = 3, binding = 0) uniform Frag {
    float sdr_ref_nits;
    // A rounded clip rect in pixel space, applied to every primitive in the
    // batch. A uniform rather than a vertex attribute because clipping changes
    // per region, not per primitive: paying 16 bytes on every glyph to avoid a
    // handful of batch breaks per frame is the wrong trade.
    float clip_enabled;
    float clip_radius;
    float pad0;
    vec4 clip_rect;   // x, y, w, h
} frag;

// Signed distance to a rounded box. p is relative to the box centre, b the
// half-extent, r the corner radius. Negative inside.
float sd_rounded_box(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

void main() {
    vec2 half_size = v_rect.zw * 0.5;
    vec2 centre = v_rect.xy + half_size;
    float radius = clamp(v_params.x, 0.0, min(half_size.x, half_size.y));
    float border_w = max(v_params.y, 0.0);
    float colorize = v_params.z;
    // Glyph quads carry their own antialiased coverage from the font
    // rasterizer. Running them through the SDF too would antialias the edge a
    // second time and thin the strokes.
    float no_sdf = v_params.w;

    float d = sd_rounded_box(v_pixel - centre, half_size, radius);

    // fwidth gives one pixel of the distance field in screen space, so the edge
    // stays a consistent ~1px regardless of scale or HiDPI factor.
    float aa = max(fwidth(d), 1e-5);

    float outer = mix(1.0 - smoothstep(-aa, aa, d), 1.0, no_sdf);
    float inner = mix(1.0 - smoothstep(-aa, aa, d + border_w), 1.0, no_sdf);

    // border_w == 0 collapses this to the fill: inner == outer, so the mix
    // returns fill and no border is drawn. No branch needed.
    vec4 col = mix(v_border, v_fill, inner);

    // Untextured primitives sample a 1x1 white pixel, so there is one code path
    // here rather than a branch -- a solid rect is the degenerate textured
    // case. `colorize` selects ColorOverlay semantics (keep the texture's
    // alpha, replace its colour) over plain modulation.
    int slot = int(v_misc.x + 0.5);
    vec4 texel = slot == 0 ? texture(tex0, v_uv)
               : slot == 1 ? texture(tex1, v_uv)
               : slot == 2 ? texture(tex2, v_uv)
                           : texture(tex3, v_uv);
    vec4 modulated = col * texel;
    vec4 colorized = vec4(col.rgb, col.a * texel.a);
    col = mix(modulated, colorized, colorize);

    float alpha = col.a * outer;

    // Perceptual coverage correction, glyphs only.
    //
    // A font rasterizer produces coverage in *gamma* space, but this pipeline
    // composites in linear light. Blending gamma-space coverage linearly makes
    // light-on-dark text render fat and dark-on-light thin.
    //
    // There is no exact fix without the destination colour, which is not
    // available mid-blend. Instead, solve for the linear-blend alpha that
    // reproduces a gamma-space blend against an assumed high-contrast
    // background -- the same approach as Skia's mask-gamma tables. Coverage 0
    // and 1 are fixed points, so solid glyph interiors are untouched.
    if (no_sdf > 0.5) {
        float coverage = texel.a;
        float cs = dot(col.rgb, vec3(0.2126, 0.7152, 0.0722));  // gamma-space luminance
        float bs = cs < 0.5 ? 1.0 : 0.0;                        // assumed background
        float cl = srgb_eotf(cs);
        float bl = bs;  // srgb_eotf is exact at 0 and 1
        float want = srgb_eotf(cs * coverage + bs * (1.0 - coverage));
        float denom = cl - bl;
        float corrected = abs(denom) > 1e-4 ? clamp((want - bl) / denom, 0.0, 1.0)
                                            : coverage;
        // v_fill.a carries the node's own opacity, which must not be gamma
        // corrected -- only the rasterizer's coverage is in the wrong space.
        alpha = v_fill.a * corrected;
    }

    // Clipping, after both alpha paths above so glyphs clip too. Antialiased
    // against the same distance field the rounded rect uses, which is what
    // makes a clipped rounded corner match a drawn one.
    if (frag.clip_enabled > 0.5) {
        vec2 clip_half = frag.clip_rect.zw * 0.5;
        vec2 clip_centre = frag.clip_rect.xy + clip_half;
        float cd = sd_rounded_box(v_pixel - clip_centre, clip_half,
                                  clamp(frag.clip_radius, 0.0,
                                        min(clip_half.x, clip_half.y)));
        float caa = max(fwidth(cd), 1e-5);
        alpha *= 1.0 - smoothstep(-caa, caa, cd);
    }

    // Straight-alpha sRGB -> linear nits at the SDR reference white every
    // SDR-referred element shares, then premultiplied for linear blending.
    // misc.y is a gain applied after the transfer function, for anything
    // that has to reach into the headroom rather than sit at SDR white.
    const float gain = max(v_misc.y, 1.0);
    vec3 nits = srgb_eotf(col.rgb) * frag.sdr_ref_nits * gain;
    out_color = vec4(nits * alpha, alpha);
}

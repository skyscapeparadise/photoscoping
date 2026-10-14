#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <vector>

#include "gpu/gpu_context.hpp"

namespace ps::gpu {

// Straight-alpha sRGB, 0..1, the space colours are authored in. Conversion to
// linear nits happens in the shader, not here.
struct Color {
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;

    // #rrggbb / #aarrggbb.
    static Color hex(std::uint32_t argb, float alpha_scale = 1.0f);

    friend bool operator==(const Color& a, const Color& b) {
        return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
    }
};

struct Rect {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
};

// How a textured primitive is drawn.
struct ImageStyle {
    // Multiplied into the sampled texel, or -- with `colorize` -- used as the
    // replacement colour while the texture supplies only alpha.
    Color tint{1.0f, 1.0f, 1.0f, 1.0f};
    float radius = 0.0f;
    // Keep the source's alpha, replace its colour: how every icon is tinted.
    bool colorize = false;
    // Multiplies the result after the transfer function, so a value above one
    // reaches into the display's headroom instead of clipping at white.
    float gain = 1.0f;
};

// How a single primitive is drawn. Every interface element is one of these:
// fill, border, radius and clipping all collapse into one signed-distance
// shader rather than four code paths.
struct RectStyle {
    Color fill;
    Color border;
    float radius = 0.0f;
    float border_width = 0.0f;
};

// Batched 2D draw list on top of SDL_GPU.
//
// This is the piece SDL_Renderer would have provided and cannot, because it
// has no custom-shader support. Primitives accumulate into a CPU-side vertex
// buffer during the frame, upload once, and draw.
//
// Compositing model: everything renders into the context's linear-light
// R16G16B16A16_FLOAT target with premultiplied alpha blending -- correct in
// linear light, which blending encoded values is not -- and a single encode
// pass converts to the swapchain at the end. Primitives never know whether
// the frame is EDR or SDR.
class DrawList {
public:
    DrawList() = default;
    ~DrawList();

    DrawList(const DrawList&) = delete;
    DrawList& operator=(const DrawList&) = delete;

    bool create(GpuContext& ctx);
    void destroy();

    // Discards the previous frame's primitives.
    //
    // `scale` is the HiDPI factor (drawable pixels per logical pixel).
    // Primitives are authored in logical units and converted to device pixels
    // here, so the SDF's fwidth-based antialiasing resolves to a crisp one
    // *device* pixel rather than a soft two.
    void begin_frame(float scale = 1.0f);

    void add_rect(const Rect& r, const RectStyle& style);
    void add_rect(const Rect& r, Color fill) { add_rect(r, RectStyle{fill}); }

    // `uv` selects a sub-rectangle of the texture in 0..1 space, so an atlased
    // icon and a whole image go through the same call. The draw list does not
    // own `texture`.
    void add_image(const Rect& r, SDL_GPUTexture* texture, const Rect& uv,
                   const ImageStyle& style);
    void add_image(const Rect& r, SDL_GPUTexture* texture, const ImageStyle& style) {
        add_image(r, texture, Rect{0.0f, 0.0f, 1.0f, 1.0f}, style);
    }

    // Text geometry as SDL3_ttf's GPU text engine produces it: indexed
    // triangles against its own glyph atlas.
    //
    // `glyph_xy`/`glyph_uv` are in *device* pixels relative to the text's own
    // origin, because fonts are opened at device size to stay crisp -- they
    // must not go through the logical-to-device scaling the other primitives
    // use. Only the origin is logical.
    //
    // Indices are expanded into the draw list's non-indexed quads. That costs a
    // third more vertices than keeping an index buffer, and buys not having a
    // second buffer to manage or a second pipeline variant.
    void add_text_geometry(SDL_GPUTexture* atlas, const SDL_FPoint* glyph_xy,
                           const SDL_FPoint* glyph_uv, const int* indices,
                           int index_count, float logical_x, float logical_y,
                           const Color& color, bool colorize);

    // Arbitrary triangles with per-vertex colour, in logical coordinates.
    // Three consecutive vertices form a triangle. These bypass the SDF; the
    // caller supplies its own antialiasing by feathering edge geometry to zero
    // alpha.
    void add_triangles(const float* xy, const Color* colors, int vertex_count);

    // Clips everything drawn until the matching pop to `r`, intersected with
    // any clip already in effect. `radius` rounds the corners.
    //
    // Nesting intersects rather than replaces, so a scroll area inside a
    // rounded panel is clipped by both. Intersecting two *rounded* rects is not
    // itself a rounded rect, so the inner radius wins and the outer contributes
    // only its bounds.
    // `r` and `radius` are in logical pixels, like every other primitive.
    void push_clip(const Rect& r, float radius = 0.0f);
    void pop_clip();

    float scale() const { return scale_; }

    // Number of draw calls the last frame needed -- one per texture change.
    std::size_t batch_count() const { return batches_.size(); }

    // Uploads this frame's vertices and draws them into the linear target,
    // then runs the encode pass into `swapchain`. Both passes are recorded
    // onto `cmd`; nothing is submitted here.
    void flush(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* swapchain,
               Uint32 width, Uint32 height);

    // The three stages of flush(), exposed so a caller can put another pass
    // between them. The footage and the panel backdrop composite into the
    // same linear target *before* the interface does, so they clear the target
    // and the interface must then load rather than clear -- otherwise the UI
    // pass erases the frame underneath it.
    void upload(SDL_GPUCommandBuffer* cmd) { upload_vertices(cmd); }
    void render_ui(SDL_GPUCommandBuffer* cmd, Uint32 width, Uint32 height, bool clear);
    void encode(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* swapchain);

    // Renders this frame to an offscreen 8-bit target and writes it to `path`
    // as a PNG. Owns its own command buffer and blocks on a fence, so it is a
    // tool for verification, not something to call per-frame.
    //
    // Always encodes through the SDR path even when the window is EDR: PNG
    // cannot represent values above white, and the SDR encode is an exact
    // round-trip, which is what makes a screenshot worth asserting against.
    // `clear_target` must be false when something has already composited into
    // the linear target this frame; otherwise the UI pass clears it and the
    // capture shows the interface over an empty frame.
    bool save_png(const char* path, Uint32 width, Uint32 height,
                  bool clear_target = true);

    // Reference white for SDR-referred content. 203 nits is the ITU-R BT.2408
    // reference, and the same number the working space puts at 1.0. Public
    // because it is a contract rather than an implementation detail: the
    // footage and backdrop passes write against it too, and a second copy of
    // it somewhere else is a drift waiting to happen.
    static constexpr float kSdrRefNits = 203.0f;

    std::size_t primitive_count() const { return vertices_.size() / kVertsPerQuad; }

private:
    // 6 vertices rather than 4-plus-indices. The index buffer would save a
    // third of the vertex bandwidth, but at UI primitive counts that is noise
    // against the added buffer to manage. Revisit if profiling says otherwise.
    static constexpr std::size_t kVertsPerQuad = 6;

    // Up to four textures are bound per batch and selected per-vertex, so a
    // run of primitives that interleaves solids, icons and glyphs still draws
    // as one call. A batch ends only when a *fifth* distinct texture appears.
    static constexpr int kTextureSlots = 4;
    // What counts as an impossible number of batches for one frame. Set well
    // above any real frame so it only ever fires on a genuine runaway.
    static constexpr std::size_t kBatchSanityLimit = 20000;

    struct Clip {
        Rect rect;
        float radius = 0.0f;
        bool enabled = false;

        friend bool operator==(const Clip& a, const Clip& b) {
            if (a.enabled != b.enabled) return false;
            if (!a.enabled) return true;
            return a.radius == b.radius && a.rect.x == b.rect.x &&
                   a.rect.y == b.rect.y && a.rect.w == b.rect.w &&
                   a.rect.h == b.rect.h;
        }
    };

    struct Batch {
        SDL_GPUTexture* textures[kTextureSlots] = {};
        int texture_count = 0;
        std::size_t first_vertex = 0;
        std::size_t vertex_count = 0;
        Clip clip;

        // Slot for `tex`, adding it if there is room; -1 when full.
        int slot_for(SDL_GPUTexture* tex) {
            for (int i = 0; i < texture_count; ++i) {
                if (textures[i] == tex) return i;
            }
            if (texture_count >= kTextureSlots) return -1;
            textures[texture_count] = tex;
            return texture_count++;
        }
    };

    struct Vertex {
        float pos[2];
        float uv[2];
        float fill[4];
        float border[4];
        float rect[4];
        float params[4];  // radius, border_width, colorize, no_sdf
        float misc[4];    // texture slot, gain, unused x2
    };
    static_assert(sizeof(Vertex) == 96, "vertex layout must match the shader attributes");

    bool create_pipelines(SDL_GPUTextureFormat swapchain_format);
    bool create_encode_pipeline(SDL_GPUTextureFormat swapchain_format);
    bool ensure_vertex_capacity(std::size_t vertex_count);
    bool ensure_capture_resources(Uint32 width, Uint32 height);
    bool create_white_texture();
    // The clip in effect, i.e. the top of the stack, or disabled when empty.
    Clip current_clip() const {
        return clip_stack_.empty() ? Clip{} : clip_stack_.back();
    }

    // Shared by flush() and save_png() so the captured image goes through the
    // same geometry and the same compositing as the on-screen frame.
    void upload_vertices(SDL_GPUCommandBuffer* cmd);
    void push_quad(const Rect& r, const Rect& uv, Vertex proto,
                   SDL_GPUTexture* texture);
    int reserve_slot(SDL_GPUTexture* texture);
    void record_ui_pass(SDL_GPUCommandBuffer* cmd, Uint32 width, Uint32 height,
                        bool clear);
    void record_encode_pass(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target,
                            SDL_GPUGraphicsPipeline* pipeline);

    GpuContext* ctx_ = nullptr;
    SDL_GPUDevice* device_ = nullptr;

    SDL_GPUGraphicsPipeline* ui_pipeline_ = nullptr;
    SDL_GPUGraphicsPipeline* encode_pipeline_ = nullptr;
    SDL_GPUSampler* linear_sampler_ = nullptr;
    std::vector<Clip> clip_stack_;
    // 1x1 opaque white. Lets the fragment shader always sample, so solid rects
    // and images share one code path instead of a branch.
    SDL_GPUTexture* white_texture_ = nullptr;

    // Created on first use -- a run that never takes a screenshot pays nothing.
    SDL_GPUGraphicsPipeline* capture_pipeline_ = nullptr;
    SDL_GPUTexture* capture_texture_ = nullptr;
    SDL_GPUTransferBuffer* capture_transfer_ = nullptr;
    Uint32 capture_w_ = 0, capture_h_ = 0;
    static constexpr SDL_GPUTextureFormat kCaptureFormat =
        SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

    SDL_GPUBuffer* vertex_buffer_ = nullptr;
    SDL_GPUTransferBuffer* transfer_buffer_ = nullptr;
    std::size_t vertex_capacity_ = 0;

    std::vector<Vertex> vertices_;
    std::vector<Batch> batches_;
    float scale_ = 1.0f;
};

}  // namespace ps::gpu

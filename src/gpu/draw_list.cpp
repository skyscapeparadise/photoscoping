#include "gpu/draw_list.hpp"

#include <cmath>

#include <algorithm>

#include <SDL3_image/SDL_image.h>

#include <cstring>
#include <string>

#include "gpu/shaders_generated.hpp"

namespace ps::gpu {

namespace {

SDL_GPUShader* create_shader(SDL_GPUDevice* device, const std::string& source,
                             SDL_GPUShaderStage stage, Uint32 num_samplers,
                             Uint32 num_uniform_buffers) {
    SDL_GPUShaderCreateInfo info = {};
    info.code = reinterpret_cast<const Uint8*>(source.c_str());
    info.code_size = source.size();
    info.entrypoint = (stage == SDL_GPU_SHADERSTAGE_VERTEX) ? "vs_main" : "fs_main";
    info.format = SDL_GPU_SHADERFORMAT_MSL;
    info.stage = stage;
    info.num_samplers = num_samplers;
    info.num_uniform_buffers = num_uniform_buffers;

    SDL_GPUShader* shader = SDL_CreateGPUShader(device, &info);
    if (!shader) {
        SDL_Log("SDL_CreateGPUShader failed: %s", SDL_GetError());
    }
    return shader;
}

struct ViewportUniform {
    float size[2];
    float pad[2];
};

struct NitsUniform {
    float sdr_ref_nits;
    float clip_enabled;
    float clip_radius;
    float pad0;
    float clip_rect[4];
};

}  // namespace

Color Color::hex(std::uint32_t argb, float alpha_scale) {
    const float a = static_cast<float>((argb >> 24) & 0xFF) / 255.0f;
    return Color{
        static_cast<float>((argb >> 16) & 0xFF) / 255.0f,
        static_cast<float>((argb >> 8) & 0xFF) / 255.0f,
        static_cast<float>(argb & 0xFF) / 255.0f,
        // A bare #rrggbb literal has no alpha byte and means opaque.
        (((argb >> 24) & 0xFF) == 0 ? 1.0f : a) * alpha_scale,
    };
}

DrawList::~DrawList() { destroy(); }

bool DrawList::create(GpuContext& ctx) {
    ctx_ = &ctx;
    device_ = ctx.device();
    if (!device_) return false;

    if (!create_pipelines(ctx.swapchain_format())) {
        destroy();
        return false;
    }

    SDL_GPUSamplerCreateInfo sampler_info = {};
    sampler_info.min_filter = SDL_GPU_FILTER_LINEAR;
    sampler_info.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampler_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    linear_sampler_ = SDL_CreateGPUSampler(device_, &sampler_info);
    if (!linear_sampler_) {
        SDL_Log("SDL_CreateGPUSampler failed: %s", SDL_GetError());
        destroy();
        return false;
    }

    if (!create_white_texture()) {
        destroy();
        return false;
    }

    return true;
}

bool DrawList::create_white_texture() {
    SDL_GPUTextureCreateInfo info = {};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = 1;
    info.height = 1;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    white_texture_ = SDL_CreateGPUTexture(device_, &info);
    if (!white_texture_) {
        SDL_Log("SDL_CreateGPUTexture (white) failed: %s", SDL_GetError());
        return false;
    }

    SDL_GPUTransferBufferCreateInfo tb = {};
    tb.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tb.size = 4;
    SDL_GPUTransferBuffer* staging = SDL_CreateGPUTransferBuffer(device_, &tb);
    if (!staging) return false;

    if (void* mapped = SDL_MapGPUTransferBuffer(device_, staging, false)) {
        const Uint8 white[4] = {255, 255, 255, 255};
        std::memcpy(mapped, white, 4);
        SDL_UnmapGPUTransferBuffer(device_, staging);
    }

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src = {};
    src.transfer_buffer = staging;
    src.pixels_per_row = 1;
    src.rows_per_layer = 1;
    SDL_GPUTextureRegion dst = {};
    dst.texture = white_texture_;
    dst.w = 1; dst.h = 1; dst.d = 1;
    SDL_UploadToGPUTexture(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(device_, staging);
    return true;
}


bool DrawList::create_encode_pipeline(SDL_GPUTextureFormat swapchain_format) {
    if (encode_pipeline_) {
        SDL_ReleaseGPUGraphicsPipeline(device_, encode_pipeline_);
        encode_pipeline_ = nullptr;
    }
    SDL_GPUShader* enc_vs = create_shader(
        device_, shaders::kEncodeVertex,
        SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
    // Both encodes normalise by the reference white; extended linear stops
    // there, SDR clamps and applies the sRGB curve.
    const char* enc_source = ctx_->mode() == ColorMode::kEDR
                                 ? shaders::kEncodeFragmentEdr
                                 : shaders::kEncodeFragmentSdr;
    SDL_GPUShader* enc_fs = create_shader(
        device_, enc_source, SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    if (!enc_vs || !enc_fs) return false;

    SDL_GPUColorTargetDescription enc_target = {};
    enc_target.format = swapchain_format;

    SDL_GPUGraphicsPipelineCreateInfo enc_info = {};
    enc_info.vertex_shader = enc_vs;
    enc_info.fragment_shader = enc_fs;
    enc_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    enc_info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    enc_info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    enc_info.target_info.color_target_descriptions = &enc_target;
    enc_info.target_info.num_color_targets = 1;

    encode_pipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &enc_info);
    SDL_ReleaseGPUShader(device_, enc_vs);
    SDL_ReleaseGPUShader(device_, enc_fs);
    if (!encode_pipeline_) {
        SDL_Log("SDL_CreateGPUGraphicsPipeline (encode) failed: %s", SDL_GetError());
        return false;
    }
    return true;
}

bool DrawList::create_pipelines(SDL_GPUTextureFormat swapchain_format) {
    // ------------------------------------------------------------ ui pass
    SDL_GPUShader* ui_vs = create_shader(
        device_, shaders::kUiVertex,
        SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    SDL_GPUShader* ui_fs = create_shader(
        device_, shaders::kUiFragment,
        SDL_GPU_SHADERSTAGE_FRAGMENT, kTextureSlots, 1);
    if (!ui_vs || !ui_fs) return false;

    SDL_GPUVertexBufferDescription vb_desc = {};
    vb_desc.slot = 0;
    vb_desc.pitch = sizeof(Vertex);
    vb_desc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    // Must match ui.vert's input locations.
    SDL_GPUVertexAttribute attrs[7] = {};
    auto set_attr = [&](int i, Uint32 loc, SDL_GPUVertexElementFormat fmt, Uint32 off) {
        attrs[i].location = loc;
        attrs[i].buffer_slot = 0;
        attrs[i].format = fmt;
        attrs[i].offset = off;
    };
    set_attr(0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(Vertex, pos));
    set_attr(1, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(Vertex, uv));
    set_attr(2, 2, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(Vertex, fill));
    set_attr(3, 3, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(Vertex, border));
    set_attr(4, 4, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(Vertex, rect));
    set_attr(5, 5, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(Vertex, params));
    set_attr(6, 6, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(Vertex, misc));

    SDL_GPUColorTargetDescription ui_target = {};
    ui_target.format = ctx_->linear_format();
    // Premultiplied alpha: the fragment shader outputs rgb already scaled by
    // alpha. Blending straight alpha here would darken edges against the
    // background, which is exactly the halo artefact this avoids.
    ui_target.blend_state.enable_blend = true;
    ui_target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    ui_target.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    ui_target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    ui_target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    ui_target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    ui_target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;

    SDL_GPUGraphicsPipelineCreateInfo ui_info = {};
    ui_info.vertex_shader = ui_vs;
    ui_info.fragment_shader = ui_fs;
    ui_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    ui_info.vertex_input_state.vertex_buffer_descriptions = &vb_desc;
    ui_info.vertex_input_state.num_vertex_buffers = 1;
    ui_info.vertex_input_state.vertex_attributes = attrs;
    ui_info.vertex_input_state.num_vertex_attributes = 7;
    ui_info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    ui_info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    ui_info.target_info.color_target_descriptions = &ui_target;
    ui_info.target_info.num_color_targets = 1;

    ui_pipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &ui_info);
    SDL_ReleaseGPUShader(device_, ui_vs);
    SDL_ReleaseGPUShader(device_, ui_fs);
    if (!ui_pipeline_) {
        SDL_Log("SDL_CreateGPUGraphicsPipeline (ui) failed: %s", SDL_GetError());
        return false;
    }

    if (!create_encode_pipeline(swapchain_format)) return false;

    return true;
}

void DrawList::begin_frame(float scale) {
    vertices_.clear();
    batches_.clear();
    clip_stack_.clear();
    scale_ = (scale > 0.0f) ? scale : 1.0f;
}

void DrawList::push_clip(const Rect& logical, float radius) {
    // Logical -> device pixels, exactly as every primitive does. The shader
    // tests the clip against v_pixel, which is device space; leaving the rect
    // in logical units silently clips to a fraction of the region on any HiDPI
    // display, and to the right one on a 1x display.
    Clip clip;
    clip.rect = Rect{logical.x * scale_, logical.y * scale_,
                     logical.w * scale_, logical.h * scale_};
    clip.radius = radius * scale_;
    clip.enabled = true;

    if (!clip_stack_.empty()) {
        // Intersect with what is already in effect. An inner region can only
        // ever shrink the visible area, never reveal what an outer clip hid.
        const Clip& outer = clip_stack_.back();
        const float x0 = std::max(clip.rect.x, outer.rect.x);
        const float y0 = std::max(clip.rect.y, outer.rect.y);
        const float x1 = std::min(clip.rect.x + clip.rect.w,
                                  outer.rect.x + outer.rect.w);
        const float y1 = std::min(clip.rect.y + clip.rect.h,
                                  outer.rect.y + outer.rect.h);
        clip.rect = Rect{x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
        if (clip.radius <= 0.0f) clip.radius = outer.radius;
    }
    clip_stack_.push_back(clip);
}

void DrawList::pop_clip() {
    if (!clip_stack_.empty()) clip_stack_.pop_back();
}

int DrawList::reserve_slot(SDL_GPUTexture* texture) {
    const Clip clip = current_clip();
    // The clip is a per-batch uniform, so a change to it ends the batch even
    // when the texture would have fitted.
    if (!batches_.empty() && batches_.back().clip == clip) {
        const int slot = batches_.back().slot_for(texture);
        if (slot >= 0) return slot;
    }
    // The first primitive, a fifth distinct texture, or a new clip region.
    Batch batch;
    batch.first_vertex = vertices_.size();
    batch.clip = clip;
    const int slot = batch.slot_for(texture);
    batches_.push_back(batch);
    return slot;
}

void DrawList::push_quad(const Rect& r, const Rect& uv, Vertex proto,
                         SDL_GPUTexture* texture) {
    proto.misc[0] = static_cast<float>(reserve_slot(texture));

    auto emit = [&](float px, float py, float u, float v) {
        Vertex out = proto;
        out.pos[0] = px; out.pos[1] = py;
        out.uv[0] = u;   out.uv[1] = v;
        vertices_.push_back(out);
    };

    const float u0 = uv.x, v0 = uv.y;
    const float u1 = uv.x + uv.w, v1 = uv.y + uv.h;

    emit(r.x,       r.y,       u0, v0);
    emit(r.x + r.w, r.y,       u1, v0);
    emit(r.x,       r.y + r.h, u0, v1);
    emit(r.x,       r.y + r.h, u0, v1);
    emit(r.x + r.w, r.y,       u1, v0);
    emit(r.x + r.w, r.y + r.h, u1, v1);

    batches_.back().vertex_count += kVertsPerQuad;
}

void DrawList::add_rect(const Rect& logical, const RectStyle& style) {
    if (logical.w <= 0.0f || logical.h <= 0.0f) return;

    const Rect r{logical.x * scale_, logical.y * scale_,
                 logical.w * scale_, logical.h * scale_};

    // The quad is expanded by a pixel on every side so the SDF has somewhere
    // to put its antialiased edge. Without this the outer half of the edge
    // gradient falls outside the geometry and corners come out visibly hard.
    constexpr float kBleed = 1.0f;
    const Rect quad{r.x - kBleed, r.y - kBleed, r.w + 2 * kBleed, r.h + 2 * kBleed};

    Vertex v = {};
    v.fill[0] = style.fill.r; v.fill[1] = style.fill.g;
    v.fill[2] = style.fill.b; v.fill[3] = style.fill.a;
    v.border[0] = style.border.r; v.border[1] = style.border.g;
    v.border[2] = style.border.b; v.border[3] = style.border.a;
    // The *unexpanded* rect -- the SDF measures against the real geometry.
    v.rect[0] = r.x; v.rect[1] = r.y; v.rect[2] = r.w; v.rect[3] = r.h;
    v.params[0] = style.radius * scale_;
    v.params[1] = style.border_width * scale_;
    v.params[2] = 0.0f;  // modulate against the 1x1 white texture

    push_quad(quad, Rect{0.0f, 0.0f, 1.0f, 1.0f}, v, white_texture_);
}

void DrawList::add_image(const Rect& logical, SDL_GPUTexture* texture,
                         const Rect& uv, const ImageStyle& style) {
    if (logical.w <= 0.0f || logical.h <= 0.0f || !texture) return;

    const Rect r{logical.x * scale_, logical.y * scale_,
                 logical.w * scale_, logical.h * scale_};

    // No bleed here. Expanding the quad would sample outside the image's UV
    // range, and with an atlas that means bleeding in a neighbouring icon.
    // Images have no SDF border, and radius clipping stays inside the rect.
    Vertex v = {};
    v.misc[1] = style.gain;
    v.fill[0] = style.tint.r; v.fill[1] = style.tint.g;
    v.fill[2] = style.tint.b; v.fill[3] = style.tint.a;
    v.border[0] = style.tint.r; v.border[1] = style.tint.g;
    v.border[2] = style.tint.b; v.border[3] = style.tint.a;
    v.rect[0] = r.x; v.rect[1] = r.y; v.rect[2] = r.w; v.rect[3] = r.h;
    v.params[0] = style.radius * scale_;
    v.params[1] = 0.0f;
    v.params[2] = style.colorize ? 1.0f : 0.0f;

    push_quad(r, uv, v, texture);
}

void DrawList::add_text_geometry(SDL_GPUTexture* atlas, const SDL_FPoint* glyph_xy,
                                 const SDL_FPoint* glyph_uv, const int* indices,
                                 int index_count, float logical_x, float logical_y,
                                 const Color& color, bool colorize) {
    if (!atlas || !glyph_xy || !glyph_uv || !indices || index_count <= 0) return;

    const int slot = reserve_slot(atlas);

    const float ox = logical_x * scale_;
    const float oy = logical_y * scale_;

    Vertex proto = {};
    proto.fill[0] = color.r; proto.fill[1] = color.g;
    proto.fill[2] = color.b; proto.fill[3] = color.a;
    proto.border[0] = color.r; proto.border[1] = color.g;
    proto.border[2] = color.b; proto.border[3] = color.a;
    proto.params[2] = colorize ? 1.0f : 0.0f;
    proto.params[3] = 1.0f;  // bypass the SDF; glyphs carry their own coverage
    proto.misc[0] = static_cast<float>(slot);

    // SDL3_ttf's GPU text engine documents Y as upwards for both vertex and
    // texture coordinates; this draw list is Y-down. In practice only the
    // *vertex* Y needs inverting -- the UVs already index the atlas top-down,
    // and flipping them as the documentation implies mirrors each glyph into
    // empty atlas space, which renders nothing at all rather than visibly
    // wrong. Verified empirically; do not "fix" the UVs to match the doc.
    //
    // The engine's block runs from 0 at the top down to -height, so inverting
    // Y alone lands it at [origin, origin + height] -- no further offset.
    for (int i = 0; i < index_count; ++i) {
        const int idx = indices[i];
        Vertex v = proto;
        v.pos[0] = ox + glyph_xy[idx].x;
        v.pos[1] = oy - glyph_xy[idx].y;
        v.uv[0] = glyph_uv[idx].x;
        v.uv[1] = glyph_uv[idx].y;
        vertices_.push_back(v);
    }
    batches_.back().vertex_count += static_cast<std::size_t>(index_count);
}

void DrawList::add_triangles(const float* xy, const Color* colors, int vertex_count) {
    if (!xy || !colors || vertex_count < 3) return;

    const int slot = reserve_slot(white_texture_);

    for (int i = 0; i < vertex_count; ++i) {
        Vertex v = {};
        v.pos[0] = xy[i * 2 + 0] * scale_;
        v.pos[1] = xy[i * 2 + 1] * scale_;
        v.uv[0] = 0.5f;
        v.uv[1] = 0.5f;  // centre of the 1x1 white texture
        v.fill[0] = colors[i].r; v.fill[1] = colors[i].g;
        v.fill[2] = colors[i].b; v.fill[3] = colors[i].a;
        v.border[0] = colors[i].r; v.border[1] = colors[i].g;
        v.border[2] = colors[i].b; v.border[3] = colors[i].a;
        v.params[3] = 1.0f;  // bypass the SDF; the path code feathers its own edges
        v.misc[0] = static_cast<float>(slot);
        vertices_.push_back(v);
    }
    batches_.back().vertex_count += static_cast<std::size_t>(vertex_count);
}

bool DrawList::ensure_vertex_capacity(std::size_t vertex_count) {
    if (vertex_count <= vertex_capacity_) return true;

    // Grow geometrically so a frame that adds primitives steadily does not
    // reallocate GPU buffers every frame.
    std::size_t new_capacity = vertex_capacity_ ? vertex_capacity_ : 4096;
    while (new_capacity < vertex_count) new_capacity *= 2;

    if (vertex_buffer_) SDL_ReleaseGPUBuffer(device_, vertex_buffer_);
    if (transfer_buffer_) SDL_ReleaseGPUTransferBuffer(device_, transfer_buffer_);
    vertex_buffer_ = nullptr;
    transfer_buffer_ = nullptr;

    const Uint32 bytes = static_cast<Uint32>(new_capacity * sizeof(Vertex));

    SDL_GPUBufferCreateInfo buf_info = {};
    buf_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    buf_info.size = bytes;
    vertex_buffer_ = SDL_CreateGPUBuffer(device_, &buf_info);
    if (!vertex_buffer_) {
        SDL_Log("SDL_CreateGPUBuffer failed: %s", SDL_GetError());
        return false;
    }

    SDL_GPUTransferBufferCreateInfo tb_info = {};
    tb_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tb_info.size = bytes;
    transfer_buffer_ = SDL_CreateGPUTransferBuffer(device_, &tb_info);
    if (!transfer_buffer_) {
        SDL_Log("SDL_CreateGPUTransferBuffer failed: %s", SDL_GetError());
        return false;
    }

    vertex_capacity_ = new_capacity;
    return true;
}

void DrawList::upload_vertices(SDL_GPUCommandBuffer* cmd) {
    if (vertices_.empty() || !ensure_vertex_capacity(vertices_.size())) return;

    void* mapped = SDL_MapGPUTransferBuffer(device_, transfer_buffer_, /*cycle=*/true);
    if (!mapped) return;
    std::memcpy(mapped, vertices_.data(), vertices_.size() * sizeof(Vertex));
    SDL_UnmapGPUTransferBuffer(device_, transfer_buffer_);

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation src = {};
    src.transfer_buffer = transfer_buffer_;
    SDL_GPUBufferRegion dst = {};
    dst.buffer = vertex_buffer_;
    dst.size = static_cast<Uint32>(vertices_.size() * sizeof(Vertex));
    SDL_UploadToGPUBuffer(copy, &src, &dst, /*cycle=*/true);
    SDL_EndGPUCopyPass(copy);
}

void DrawList::record_ui_pass(SDL_GPUCommandBuffer* cmd, Uint32 width, Uint32 height,
                              bool clear) {
    SDL_GPUColorTargetInfo target = {};
    target.texture = ctx_->linear_target();
    target.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 0.0f};
    target.load_op = clear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
    target.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
    if (!vertices_.empty() && vertex_buffer_) {
        SDL_BindGPUGraphicsPipeline(pass, ui_pipeline_);

        SDL_GPUBufferBinding vb = {};
        vb.buffer = vertex_buffer_;
        SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);

        ViewportUniform vu = {};
        vu.size[0] = static_cast<float>(width);
        vu.size[1] = static_cast<float>(height);
        SDL_PushGPUVertexUniformData(cmd, 0, &vu, sizeof(vu));

        // Safety valve. A batch is a sampler bind plus a draw call, and a busy
        // editor frame is a few hundred; tens of thousands means something
        // upstream is breaking the batch on every single primitive. The usual
        // cause is a clip rect that compares unequal to itself -- operator==
        // tests floats, and NaN fails that test -- so reserve_slot can never
        // reuse the batch it just pushed. Binding them all wedges the main
        // thread inside Metal's retain traffic with no way out, so say what
        // happened and drop the frame instead of freezing the editor.
        if (batches_.size() > kBatchSanityLimit) {
            static bool reported = false;
            if (!reported) {
                reported = true;
                const Clip& c = batches_.front().clip;
                SDL_Log("draw list ran away: %zu batches over %zu vertices; "
                        "first clip %s [%g %g %g %g] r=%g -- frame dropped",
                        batches_.size(), vertices_.size(),
                        c.enabled ? "on" : "off", c.rect.x, c.rect.y, c.rect.w,
                        c.rect.h, c.radius);
            }
            SDL_EndGPURenderPass(pass);
            return;
        }

        // One draw per texture. Primitives keep their submission order, so
        // z-order is preserved and overlapping images still composite
        // correctly -- batching never reorders, it only merges neighbours.
        for (const Batch& batch : batches_) {
            // Every declared sampler must be bound even when the batch uses
            // fewer; unused slots get the 1x1 white texture rather than null.
            SDL_GPUTextureSamplerBinding bindings[kTextureSlots];
            for (int i = 0; i < kTextureSlots; ++i) {
                bindings[i].texture = (i < batch.texture_count) ? batch.textures[i]
                                                                : white_texture_;
                bindings[i].sampler = linear_sampler_;
            }
            SDL_BindGPUFragmentSamplers(pass, 0, bindings, kTextureSlots);

            // Pushed per batch rather than once, because the clip rect lives
            // in it and is what a batch break most often means.
            NitsUniform nu = {};
            nu.sdr_ref_nits = kSdrRefNits;
            nu.clip_enabled = batch.clip.enabled ? 1.0f : 0.0f;
            nu.clip_radius = batch.clip.radius;
            nu.clip_rect[0] = batch.clip.rect.x;
            nu.clip_rect[1] = batch.clip.rect.y;
            nu.clip_rect[2] = batch.clip.rect.w;
            nu.clip_rect[3] = batch.clip.rect.h;
            SDL_PushGPUFragmentUniformData(cmd, 0, &nu, sizeof(nu));

            SDL_DrawGPUPrimitives(pass, static_cast<Uint32>(batch.vertex_count), 1,
                                  static_cast<Uint32>(batch.first_vertex), 0);
        }
    }
    SDL_EndGPURenderPass(pass);
}

void DrawList::record_encode_pass(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target,
                                  SDL_GPUGraphicsPipeline* pipeline) {
    SDL_GPUColorTargetInfo info = {};
    info.texture = target;
    info.load_op = SDL_GPU_LOADOP_DONT_CARE;
    info.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &info, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(pass, pipeline);

    SDL_GPUTextureSamplerBinding binding = {};
    binding.texture = ctx_->linear_target();
    binding.sampler = linear_sampler_;
    SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);

    NitsUniform nu = {};
    nu.sdr_ref_nits = kSdrRefNits;
    SDL_PushGPUFragmentUniformData(cmd, 0, &nu, sizeof(nu));

    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
}

void DrawList::render_ui(SDL_GPUCommandBuffer* cmd, Uint32 width, Uint32 height,
                         bool clear) {
    record_ui_pass(cmd, width, height, clear);
}

void DrawList::encode(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* swapchain) {
    record_encode_pass(cmd, swapchain, encode_pipeline_);
}

void DrawList::flush(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* swapchain,
                     Uint32 width, Uint32 height) {
    upload(cmd);
    render_ui(cmd, width, height, /*clear=*/true);
    encode(cmd, swapchain);
}

bool DrawList::ensure_capture_resources(Uint32 width, Uint32 height) {
    if (!capture_pipeline_) {
        SDL_GPUShader* vs = create_shader(
            device_, shaders::kEncodeVertex,
            SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
        SDL_GPUShader* fs = create_shader(
            device_, shaders::kEncodeFragmentSdr,
            SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
        if (!vs || !fs) return false;

        SDL_GPUColorTargetDescription target = {};
        target.format = kCaptureFormat;

        SDL_GPUGraphicsPipelineCreateInfo info = {};
        info.vertex_shader = vs;
        info.fragment_shader = fs;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.target_info.color_target_descriptions = &target;
        info.target_info.num_color_targets = 1;

        capture_pipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &info);
        SDL_ReleaseGPUShader(device_, vs);
        SDL_ReleaseGPUShader(device_, fs);
        if (!capture_pipeline_) {
            SDL_Log("SDL_CreateGPUGraphicsPipeline (capture) failed: %s", SDL_GetError());
            return false;
        }
    }

    if (capture_texture_ && capture_w_ == width && capture_h_ == height) return true;

    if (capture_texture_) SDL_ReleaseGPUTexture(device_, capture_texture_);
    if (capture_transfer_) SDL_ReleaseGPUTransferBuffer(device_, capture_transfer_);
    capture_texture_ = nullptr;
    capture_transfer_ = nullptr;

    SDL_GPUTextureCreateInfo tex = {};
    tex.type = SDL_GPU_TEXTURETYPE_2D;
    tex.format = kCaptureFormat;
    tex.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    tex.width = width;
    tex.height = height;
    tex.layer_count_or_depth = 1;
    tex.num_levels = 1;
    tex.sample_count = SDL_GPU_SAMPLECOUNT_1;
    capture_texture_ = SDL_CreateGPUTexture(device_, &tex);
    if (!capture_texture_) {
        SDL_Log("SDL_CreateGPUTexture (capture) failed: %s", SDL_GetError());
        return false;
    }

    SDL_GPUTransferBufferCreateInfo tb = {};
    tb.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tb.size = width * height * 4;
    capture_transfer_ = SDL_CreateGPUTransferBuffer(device_, &tb);
    if (!capture_transfer_) {
        SDL_Log("SDL_CreateGPUTransferBuffer (capture) failed: %s", SDL_GetError());
        return false;
    }

    capture_w_ = width;
    capture_h_ = height;
    return true;
}

bool DrawList::save_png(const char* path, Uint32 width, Uint32 height,
                        bool clear_target) {
    if (!ensure_capture_resources(width, height)) return false;

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (!cmd) {
        SDL_Log("SDL_AcquireGPUCommandBuffer (capture) failed: %s", SDL_GetError());
        return false;
    }

    upload_vertices(cmd);
    record_ui_pass(cmd, width, height, clear_target);
    record_encode_pass(cmd, capture_texture_, capture_pipeline_);

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion region = {};
    region.texture = capture_texture_;
    region.w = width;
    region.h = height;
    region.d = 1;
    SDL_GPUTextureTransferInfo dst = {};
    dst.transfer_buffer = capture_transfer_;
    dst.pixels_per_row = width;
    dst.rows_per_layer = height;
    SDL_DownloadFromGPUTexture(copy, &region, &dst);
    SDL_EndGPUCopyPass(copy);

    // Blocking is the point here: a screenshot is a synchronous tool.
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (!fence) {
        SDL_Log("SDL_SubmitGPUCommandBufferAndAcquireFence failed: %s", SDL_GetError());
        return false;
    }
    SDL_WaitForGPUFences(device_, true, &fence, 1);
    SDL_ReleaseGPUFence(device_, fence);

    void* pixels = SDL_MapGPUTransferBuffer(device_, capture_transfer_, /*cycle=*/false);
    if (!pixels) {
        SDL_Log("SDL_MapGPUTransferBuffer (capture) failed: %s", SDL_GetError());
        return false;
    }

    SDL_Surface* surface = SDL_CreateSurfaceFrom(
        static_cast<int>(width), static_cast<int>(height),
        SDL_PIXELFORMAT_ABGR8888,  // R8G8B8A8_UNORM in SDL's little-endian naming
        pixels, static_cast<int>(width * 4));
    bool ok = false;
    if (surface) {
        ok = IMG_SavePNG(surface, path);
        if (!ok) SDL_Log("IMG_SavePNG failed: %s", SDL_GetError());
        SDL_DestroySurface(surface);
    } else {
        SDL_Log("SDL_CreateSurfaceFrom failed: %s", SDL_GetError());
    }
    SDL_UnmapGPUTransferBuffer(device_, capture_transfer_);
    return ok;
}

void DrawList::destroy() {
    if (!device_) return;
    if (vertex_buffer_) { SDL_ReleaseGPUBuffer(device_, vertex_buffer_); vertex_buffer_ = nullptr; }
    if (transfer_buffer_) { SDL_ReleaseGPUTransferBuffer(device_, transfer_buffer_); transfer_buffer_ = nullptr; }
    if (linear_sampler_) { SDL_ReleaseGPUSampler(device_, linear_sampler_); linear_sampler_ = nullptr; }
    if (white_texture_) { SDL_ReleaseGPUTexture(device_, white_texture_); white_texture_ = nullptr; }
    if (ui_pipeline_) { SDL_ReleaseGPUGraphicsPipeline(device_, ui_pipeline_); ui_pipeline_ = nullptr; }
    if (encode_pipeline_) { SDL_ReleaseGPUGraphicsPipeline(device_, encode_pipeline_); encode_pipeline_ = nullptr; }
    if (capture_pipeline_) { SDL_ReleaseGPUGraphicsPipeline(device_, capture_pipeline_); capture_pipeline_ = nullptr; }
    if (capture_texture_) { SDL_ReleaseGPUTexture(device_, capture_texture_); capture_texture_ = nullptr; }
    if (capture_transfer_) { SDL_ReleaseGPUTransferBuffer(device_, capture_transfer_); capture_transfer_ = nullptr; }
    capture_w_ = capture_h_ = 0;
    vertex_capacity_ = 0;
    device_ = nullptr;
    ctx_ = nullptr;
}

}  // namespace ps::gpu

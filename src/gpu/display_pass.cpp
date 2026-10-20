#include "gpu/display_pass.hpp"

#include <algorithm>

#include "color/color_space.hpp"
#include "gpu/draw_list.hpp"
#include "gpu/gpu_context.hpp"
#include "gpu/pipeline.hpp"
#include "gpu/shaders_generated.hpp"

namespace ps::gpu {

namespace {

// std140 layout of display.frag's Display block.
struct DisplayUniform {
    float to_display[3][4];
    float wash[4];
    float nits_per_unit;
    float view;
    float checker_px;
    float has_fixes;
};

// The overlay's wash: the nebula's plum, linear Rec.709, over most of what
// the matte drops so the footage still shows faintly through it.
constexpr float kWash[4] = {0.120f, 0.012f, 0.150f, 0.72f};
constexpr float kCheckerLogical = 12.0f;

}  // namespace

DisplayPass::~DisplayPass() { destroy(); }

bool DisplayPass::create(const GpuContext& gpu) {
    device_ = gpu.device();
    pipeline_ = create_quad_pipeline(device_, shaders::kDisplayFragment, 4, gpu.linear_format());
    // Linear filtering for now; an 8K frame in a small viewer aliases, which
    // a mip chain or a proper downscale will fix when the viewer gets one.
    sampler_ = create_sampler(device_, SDL_GPU_FILTER_LINEAR);
    nearest_ = create_sampler(device_, SDL_GPU_FILTER_NEAREST);
    return pipeline_ && sampler_ && nearest_;
}

void DisplayPass::destroy() {
    if (!device_) return;
    if (pipeline_) SDL_ReleaseGPUGraphicsPipeline(device_, pipeline_);
    if (sampler_) SDL_ReleaseGPUSampler(device_, sampler_);
    if (nearest_) SDL_ReleaseGPUSampler(device_, nearest_);
    pipeline_ = nullptr;
    sampler_ = nullptr;
    nearest_ = nullptr;
    device_ = nullptr;
}

void DisplayPass::draw(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, const SDL_FRect& area,
                       const SDL_FRect& image, const Frame& frame, DisplayView view, float scale,
                       bool nearest, SDL_FColor background) {
    SDL_GPUColorTargetInfo info = {};
    info.texture = target;
    info.clear_color = background;
    info.load_op = SDL_GPU_LOADOP_CLEAR;
    info.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &info, 1, nullptr);
    if (frame.working && frame.width && frame.height && area.w >= 1.0f && area.h >= 1.0f) {
        if (!frame.matte || (view == DisplayView::kSubject && !frame.subject))
            view = DisplayView::kFootage;

        // The viewport is the viewer's area, and clips to it; the image is
        // placed within it in the viewport's NDC, y up, so its top edge is
        // the larger number.
        const SDL_GPUViewport viewport = {area.x, area.y, area.w, area.h, 0.0f, 1.0f};
        SDL_SetGPUViewport(pass, &viewport);
        const float x0 = (image.x - area.x) / area.w * 2.0f - 1.0f;
        const float x1 = (image.x + image.w - area.x) / area.w * 2.0f - 1.0f;
        const float y_top = 1.0f - (image.y - area.y) / area.h * 2.0f;
        const float y_bottom = 1.0f - (image.y + image.h - area.y) / area.h * 2.0f;

        DisplayUniform u = {};
        const color::Mat3 m = color::working_to_rec709();
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) u.to_display[r][c] = m[r * 3 + c];
        for (int i = 0; i < 4; ++i) u.wash[i] = kWash[i];
        // The working space puts reference white at 1.0, and the linear
        // target holds nits against the same reference the interface uses.
        u.nits_per_unit = DrawList::kSdrRefNits;
        u.view = static_cast<float>(view);
        u.checker_px = kCheckerLogical * scale;
        u.has_fixes = frame.fixes ? 1.0f : 0.0f;

        SDL_BindGPUGraphicsPipeline(pass, pipeline_);
        // Every declared sampler must be bound; a texture a view does not
        // read stands in with the footage.
        SDL_GPUSampler* sampler = nearest ? nearest_ : sampler_;
        // The fixes are always filtered: they are half the frame's
        // resolution, and their edges are meant to be soft.
        const SDL_GPUTextureSamplerBinding bindings[4] = {
            {frame.working, sampler},
            {frame.matte ? frame.matte : frame.working, sampler},
            {frame.subject ? frame.subject : frame.working, sampler},
            {frame.fixes ? frame.fixes : frame.working, sampler_},
        };
        SDL_BindGPUFragmentSamplers(pass, 0, bindings, 4);
        SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof u);
        draw_quad(cmd, pass, QuadRect{{x0, y_bottom, x1, y_top}});
    }
    SDL_EndGPURenderPass(pass);
}

}  // namespace ps::gpu

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
    float pad;
};

// The overlay's wash: the nebula's plum, linear Rec.709, over most of what
// the matte drops so the footage still shows faintly through it.
constexpr float kWash[4] = {0.120f, 0.012f, 0.150f, 0.72f};
constexpr float kCheckerLogical = 12.0f;

}  // namespace

DisplayPass::~DisplayPass() { destroy(); }

bool DisplayPass::create(const GpuContext& gpu) {
    device_ = gpu.device();
    pipeline_ = create_quad_pipeline(device_, shaders::kDisplayFragment, 3, gpu.linear_format());
    // Linear filtering for now; an 8K frame in a small viewer aliases, which
    // a mip chain or a proper downscale will fix when the viewer gets one.
    sampler_ = create_sampler(device_, SDL_GPU_FILTER_LINEAR);
    return pipeline_ && sampler_;
}

void DisplayPass::destroy() {
    if (!device_) return;
    if (pipeline_) SDL_ReleaseGPUGraphicsPipeline(device_, pipeline_);
    if (sampler_) SDL_ReleaseGPUSampler(device_, sampler_);
    pipeline_ = nullptr;
    sampler_ = nullptr;
    device_ = nullptr;
}

void DisplayPass::draw(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, const SDL_FRect& area,
                       const Frame& frame, DisplayView view, float scale,
                       SDL_FColor background) {
    SDL_GPUColorTargetInfo info = {};
    info.texture = target;
    info.clear_color = background;
    info.load_op = SDL_GPU_LOADOP_CLEAR;
    info.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &info, 1, nullptr);
    if (frame.working && frame.width && frame.height && area.w >= 1.0f && area.h >= 1.0f) {
        if (!frame.matte || (view == DisplayView::kSubject && !frame.subject))
            view = DisplayView::kFootage;

        // The viewport is the viewer's area; within it, fit and centre with
        // the aspect preserved. Quad coordinates are NDC of the viewport.
        const SDL_GPUViewport viewport = {area.x, area.y, area.w, area.h, 0.0f, 1.0f};
        SDL_SetGPUViewport(pass, &viewport);
        const float fit = std::min(area.w / frame.width, area.h / frame.height);
        const float sx = frame.width * fit / area.w;
        const float sy = frame.height * fit / area.h;

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

        SDL_BindGPUGraphicsPipeline(pass, pipeline_);
        // Every declared sampler must be bound; a texture a view does not
        // read stands in with the footage.
        const SDL_GPUTextureSamplerBinding bindings[3] = {
            {frame.working, sampler_},
            {frame.matte ? frame.matte : frame.working, sampler_},
            {frame.subject ? frame.subject : frame.working, sampler_},
        };
        SDL_BindGPUFragmentSamplers(pass, 0, bindings, 3);
        SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof u);
        draw_quad(cmd, pass, QuadRect{{-sx, -sy, sx, sy}});
    }
    SDL_EndGPURenderPass(pass);
}

}  // namespace ps::gpu

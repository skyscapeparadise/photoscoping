#include "gpu/nebula_pass.hpp"

#include "gpu/draw_list.hpp"
#include "gpu/gpu_context.hpp"
#include "gpu/pipeline.hpp"
#include "gpu/shaders_generated.hpp"

namespace ps::gpu {

namespace {

// std140 layout of nebula.frag's Nebula block.
struct NebulaUniform {
    float size[2];
    float panel_width;
    float sdr_ref_nits;
};

}  // namespace

NebulaPass::~NebulaPass() { destroy(); }

bool NebulaPass::create(const GpuContext& gpu) {
    device_ = gpu.device();
    pipeline_ = create_quad_pipeline(device_, shaders::kNebulaFragment, 0, gpu.linear_format());
    return pipeline_ != nullptr;
}

void NebulaPass::destroy() {
    if (!device_) return;
    if (pipeline_) SDL_ReleaseGPUGraphicsPipeline(device_, pipeline_);
    pipeline_ = nullptr;
    device_ = nullptr;
}

void NebulaPass::draw(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, const SDL_FRect& area,
                      float logical_w, float logical_h, float panel_w) {
    if (area.w < 1.0f || area.h < 1.0f) return;
    SDL_GPUColorTargetInfo info = {};
    info.texture = target;
    info.load_op = SDL_GPU_LOADOP_LOAD;
    info.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &info, 1, nullptr);
    const SDL_GPUViewport viewport = {area.x, area.y, area.w, area.h, 0.0f, 1.0f};
    SDL_SetGPUViewport(pass, &viewport);

    NebulaUniform u = {};
    u.size[0] = logical_w;
    u.size[1] = logical_h;
    u.panel_width = panel_w;
    u.sdr_ref_nits = DrawList::kSdrRefNits;
    SDL_BindGPUGraphicsPipeline(pass, pipeline_);
    SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof u);
    draw_quad(cmd, pass, QuadRect{});
    SDL_EndGPURenderPass(pass);
}

}  // namespace ps::gpu

#include "gpu/pipeline.hpp"

#include <cstring>

#include "gpu/shaders_generated.hpp"

namespace ps::gpu {

namespace {

SDL_GPUShader* create_shader(SDL_GPUDevice* device, const char* msl, SDL_GPUShaderStage stage,
                             Uint32 samplers, Uint32 uniform_buffers) {
    SDL_GPUShaderCreateInfo info = {};
    info.code = reinterpret_cast<const Uint8*>(msl);
    info.code_size = std::strlen(msl);
    info.entrypoint = stage == SDL_GPU_SHADERSTAGE_VERTEX ? "vs_main" : "fs_main";
    info.format = SDL_GPU_SHADERFORMAT_MSL;
    info.stage = stage;
    info.num_samplers = samplers;
    info.num_uniform_buffers = uniform_buffers;
    SDL_GPUShader* shader = SDL_CreateGPUShader(device, &info);
    if (!shader) SDL_Log("SDL_CreateGPUShader failed: %s", SDL_GetError());
    return shader;
}

}  // namespace

SDL_GPUGraphicsPipeline* create_quad_pipeline(SDL_GPUDevice* device, const char* fragment_msl,
                                              Uint32 fragment_samplers,
                                              SDL_GPUTextureFormat format) {
    SDL_GPUShader* vertex =
        create_shader(device, shaders::kQuadVertex, SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    SDL_GPUShader* fragment = create_shader(device, fragment_msl, SDL_GPU_SHADERSTAGE_FRAGMENT,
                                            fragment_samplers, 1);
    SDL_GPUGraphicsPipeline* pipeline = nullptr;
    if (vertex && fragment) {
        SDL_GPUColorTargetDescription target = {};
        target.format = format;
        SDL_GPUGraphicsPipelineCreateInfo info = {};
        info.vertex_shader = vertex;
        info.fragment_shader = fragment;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.target_info.color_target_descriptions = &target;
        info.target_info.num_color_targets = 1;
        pipeline = SDL_CreateGPUGraphicsPipeline(device, &info);
        if (!pipeline) SDL_Log("SDL_CreateGPUGraphicsPipeline failed: %s", SDL_GetError());
    }
    // A pipeline keeps what it needs; the shader objects can go.
    if (vertex) SDL_ReleaseGPUShader(device, vertex);
    if (fragment) SDL_ReleaseGPUShader(device, fragment);
    return pipeline;
}

SDL_GPUSampler* create_sampler(SDL_GPUDevice* device, SDL_GPUFilter filter) {
    SDL_GPUSamplerCreateInfo info = {};
    info.min_filter = filter;
    info.mag_filter = filter;
    info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    SDL_GPUSampler* sampler = SDL_CreateGPUSampler(device, &info);
    if (!sampler) SDL_Log("SDL_CreateGPUSampler failed: %s", SDL_GetError());
    return sampler;
}

void draw_quad(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass, const QuadRect& rect) {
    SDL_PushGPUVertexUniformData(cmd, 0, rect.rect, sizeof rect.rect);
    SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
}

}  // namespace ps::gpu

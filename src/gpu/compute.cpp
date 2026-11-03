#include "gpu/compute.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace ps::gpu {

Buffer::~Buffer() { release(); }

Buffer::Buffer(Buffer&& other) noexcept
    : device_(std::exchange(other.device_, nullptr)),
      buffer_(std::exchange(other.buffer_, nullptr)),
      size_(std::exchange(other.size_, 0)) {}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        release();
        device_ = std::exchange(other.device_, nullptr);
        buffer_ = std::exchange(other.buffer_, nullptr);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

bool Buffer::reserve(SDL_GPUDevice* device, std::size_t bytes) {
    bytes = std::max<std::size_t>(bytes, 16);
    if (buffer_ && size_ >= bytes && device_ == device) return true;
    release();
    SDL_GPUBufferCreateInfo info = {};
    info.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
    info.size = static_cast<Uint32>(bytes);
    if (bytes > 0xFFFFFFFFu) {
        SDL_Log("a GPU buffer of %zu bytes is more than SDL_GPU can make", bytes);
        return false;
    }
    buffer_ = SDL_CreateGPUBuffer(device, &info);
    if (!buffer_) {
        SDL_Log("SDL_CreateGPUBuffer failed: %s", SDL_GetError());
        return false;
    }
    device_ = device;
    size_ = bytes;
    return true;
}

void Buffer::release() {
    if (buffer_) SDL_ReleaseGPUBuffer(device_, buffer_);
    buffer_ = nullptr;
    size_ = 0;
}

Kernel create_kernel(SDL_GPUDevice* device, const char* msl, const KernelLayout& layout) {
    SDL_GPUComputePipelineCreateInfo info = {};
    info.code = reinterpret_cast<const Uint8*>(msl);
    info.code_size = std::strlen(msl);
    info.entrypoint = "cs_main";
    info.format = SDL_GPU_SHADERFORMAT_MSL;
    info.num_samplers = layout.samplers;
    info.num_readonly_storage_buffers = layout.readonly_buffers;
    info.num_readwrite_storage_buffers = layout.readwrite_buffers;
    info.num_uniform_buffers = layout.uniform_buffers;
    info.threadcount_x = layout.group_x;
    info.threadcount_y = layout.group_y;
    info.threadcount_z = 1;
    Kernel kernel;
    kernel.pipeline = SDL_CreateGPUComputePipeline(device, &info);
    if (!kernel.pipeline) SDL_Log("SDL_CreateGPUComputePipeline failed: %s", SDL_GetError());
    kernel.group_x = layout.group_x;
    kernel.group_y = layout.group_y;
    return kernel;
}

void release_kernel(SDL_GPUDevice* device, Kernel& kernel) {
    if (kernel.pipeline) SDL_ReleaseGPUComputePipeline(device, kernel.pipeline);
    kernel.pipeline = nullptr;
}

void dispatch(SDL_GPUCommandBuffer* cmd, const Dispatch& d) {
    SDL_GPUStorageBufferReadWriteBinding rw[8] = {};
    Uint32 nrw = 0;
    for (const Buffer* b : d.write) rw[nrw++] = {b->get(), false, {}};
    SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, nullptr, 0, rw, nrw);
    SDL_BindGPUComputePipeline(pass, d.kernel->pipeline);
    if (d.texture) {
        SDL_GPUTextureSamplerBinding t = {d.texture, d.sampler};
        SDL_BindGPUComputeSamplers(pass, 0, &t, 1);
    }
    SDL_GPUBuffer* ro[8] = {};
    Uint32 nro = 0;
    for (const Buffer* b : d.read) ro[nro++] = b->get();
    if (nro) SDL_BindGPUComputeStorageBuffers(pass, 0, ro, nro);
    if (d.uniforms) SDL_PushGPUComputeUniformData(cmd, 0, d.uniforms, d.uniform_bytes);
    const Uint32 gx = (d.width + d.kernel->group_x - 1) / d.kernel->group_x;
    const Uint32 gy = (d.height + d.kernel->group_y - 1) / d.kernel->group_y;
    SDL_DispatchGPUCompute(pass, gx, gy, 1);
    SDL_EndGPUComputePass(pass);
}

Transfer::~Transfer() {
    if (up_) SDL_ReleaseGPUTransferBuffer(device_, up_);
    if (down_) SDL_ReleaseGPUTransferBuffer(device_, down_);
}

bool Transfer::staging(std::size_t bytes, bool upload) {
    SDL_GPUTransferBuffer*& buffer = upload ? up_ : down_;
    std::size_t& size = upload ? up_size_ : down_size_;
    if (buffer && size >= bytes) return true;
    if (buffer) SDL_ReleaseGPUTransferBuffer(device_, buffer);
    SDL_GPUTransferBufferCreateInfo info = {};
    info.usage = upload ? SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD : SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    info.size = static_cast<Uint32>(bytes);
    buffer = SDL_CreateGPUTransferBuffer(device_, &info);
    size = buffer ? bytes : 0;
    if (!buffer) SDL_Log("SDL_CreateGPUTransferBuffer failed: %s", SDL_GetError());
    return buffer != nullptr;
}

bool Transfer::upload(Buffer& to, const void* data, std::size_t bytes) {
    if (!to.get() || to.size() < bytes || !staging(bytes, true)) return false;
    void* mapped = SDL_MapGPUTransferBuffer(device_, up_, /*cycle=*/true);
    if (!mapped) return false;
    std::memcpy(mapped, data, bytes);
    SDL_UnmapGPUTransferBuffer(device_, up_);
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (!cmd) return false;
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation src = {up_, 0};
    SDL_GPUBufferRegion dst = {to.get(), 0, static_cast<Uint32>(bytes)};
    SDL_UploadToGPUBuffer(copy, &src, &dst, /*cycle=*/false);
    SDL_EndGPUCopyPass(copy);
    // Submitted, not waited for: later work on the same queue sees it.
    return SDL_SubmitGPUCommandBuffer(cmd);
}

bool Transfer::download(const Buffer& from, void* data, std::size_t bytes) {
    if (!from.get() || from.size() < bytes || !staging(bytes, false)) return false;
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (!cmd) return false;
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUBufferRegion src = {from.get(), 0, static_cast<Uint32>(bytes)};
    SDL_GPUTransferBufferLocation dst = {down_, 0};
    SDL_DownloadFromGPUBuffer(copy, &src, &dst);
    SDL_EndGPUCopyPass(copy);
    if (!submit_and_wait(device_, cmd)) return false;
    void* mapped = SDL_MapGPUTransferBuffer(device_, down_, false);
    if (!mapped) return false;
    std::memcpy(data, mapped, bytes);
    SDL_UnmapGPUTransferBuffer(device_, down_);
    return true;
}

bool submit_and_wait(SDL_GPUDevice* device, SDL_GPUCommandBuffer* cmd) {
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (!fence) return false;
    const bool ok = SDL_WaitForGPUFences(device, true, &fence, 1);
    SDL_ReleaseGPUFence(device, fence);
    return ok;
}

}  // namespace ps::gpu

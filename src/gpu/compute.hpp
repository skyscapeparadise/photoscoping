#pragma once

// Compute on the GPU: storage buffers, kernels, and the passes between them.
//
// Image work runs here rather than on the CPU (photoscoping is GPU first).
// Buffers hold plain arrays -- planar floats, mostly -- so a kernel ports a
// CPU loop index for index, and a result can be checked against the CPU
// version it replaces.
//
// Binding order is SDL_CreateGPUComputePipeline's: a kernel's uniform block
// is binding 0, its read-only buffers 1.., its read-write buffers after (see
// tools/build_shaders.py).

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace ps::gpu {

// A storage buffer, readable and writable by kernels.
class Buffer {
public:
    Buffer() = default;
    ~Buffer();
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;

    // At least `bytes` long; kept if it already is. False if it cannot be made.
    bool reserve(SDL_GPUDevice* device, std::size_t bytes);
    void release();
    SDL_GPUBuffer* get() const { return buffer_; }
    std::size_t size() const { return size_; }

private:
    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUBuffer* buffer_ = nullptr;
    std::size_t size_ = 0;
};

// A compiled kernel: its pipeline and the work-group size it was written for.
struct Kernel {
    SDL_GPUComputePipeline* pipeline = nullptr;
    Uint32 group_x = 16, group_y = 16;
    explicit operator bool() const { return pipeline != nullptr; }
};

struct KernelLayout {
    Uint32 samplers = 0;          // sampled textures, each with its sampler
    Uint32 readonly_buffers = 0;
    Uint32 readwrite_buffers = 0;
    Uint32 uniform_buffers = 1;
    Uint32 group_x = 16, group_y = 16;
};

Kernel create_kernel(SDL_GPUDevice* device, const char* msl, const KernelLayout& layout);
void release_kernel(SDL_GPUDevice* device, Kernel& kernel);

// One dispatch, in its own pass, over a width x height grid of threads. The
// pass boundary is the barrier: what one dispatch writes, the next reads.
struct Dispatch {
    const Kernel* kernel = nullptr;
    std::initializer_list<const Buffer*> read;
    std::initializer_list<const Buffer*> write;
    const void* uniforms = nullptr;
    Uint32 uniform_bytes = 0;
    SDL_GPUTexture* texture = nullptr;  // sampled, slot 0
    SDL_GPUSampler* sampler = nullptr;
    Uint32 width = 1, height = 1;
};
void dispatch(SDL_GPUCommandBuffer* cmd, const Dispatch& d);

// Copies to and from buffers. Each waits for the GPU: an upload is recorded
// in its own command buffer and submitted, a download waits for everything
// already submitted that writes the buffer.
class Transfer {
public:
    explicit Transfer(SDL_GPUDevice* device) : device_(device) {}
    ~Transfer();
    Transfer(const Transfer&) = delete;
    Transfer& operator=(const Transfer&) = delete;

    bool upload(Buffer& to, const void* data, std::size_t bytes);
    bool download(const Buffer& from, void* data, std::size_t bytes);

    template <class T>
    bool upload(Buffer& to, const std::vector<T>& data) {
        return upload(to, data.data(), data.size() * sizeof(T));
    }
    template <class T>
    bool download(const Buffer& from, std::vector<T>& data) {
        return download(from, data.data(), data.size() * sizeof(T));
    }

private:
    bool staging(std::size_t bytes, bool upload);

    SDL_GPUDevice* device_;
    SDL_GPUTransferBuffer* up_ = nullptr;
    SDL_GPUTransferBuffer* down_ = nullptr;
    std::size_t up_size_ = 0, down_size_ = 0;
};

// Submits and waits: everything recorded in `cmd` is done on return.
bool submit_and_wait(SDL_GPUDevice* device, SDL_GPUCommandBuffer* cmd);

}  // namespace ps::gpu

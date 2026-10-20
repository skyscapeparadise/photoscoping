#include "app/matte_frames.hpp"

#include <SDL3_image/SDL_image.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace ps::app {

namespace {

namespace fs = std::filesystem;

// Exactly six digits and .png: a frame. Anything else -- the .partial.png a
// frame is written under before it is renamed into place -- is not.
bool frame_of(const std::string& name, std::int64_t* index) {
    if (name.size() != 10 || name.compare(6, 4, ".png") != 0) return false;
    std::int64_t value = 0;
    for (int i = 0; i < 6; ++i) {
        if (name[i] < '0' || name[i] > '9') return false;
        value = value * 10 + (name[i] - '0');
    }
    *index = value;
    return true;
}

std::string frame_path(const std::string& dir, std::int64_t frame) {
    char name[16];
    std::snprintf(name, sizeof name, "%06lld.png", static_cast<long long>(frame));
    return (fs::path(dir) / name).string();
}

}  // namespace

MatteFrames::~MatteFrames() { destroy(); }

void MatteFrames::open(SDL_GPUDevice* device, const std::string& dir, std::int64_t frame_count) {
    destroy();
    device_ = device;
    dir_ = dir;
    done_.assign(static_cast<std::size_t>(std::max<std::int64_t>(0, frame_count)), false);
    rescan();
    // On the worker: read the PNG and bring it to RGBA bytes, coverage in
    // red, so the main thread only copies it. Whether the file is there is
    // asked of the disk, not of done_, which the main thread rescans.
    start([this](std::int64_t frame) {
        SDL_Surface* file = IMG_Load(frame_path(dir_, frame).c_str());
        if (!file) return false;
        SDL_Surface* rgba = SDL_ConvertSurface(file, SDL_PIXELFORMAT_ABGR8888);
        SDL_DestroySurface(file);
        if (!rgba) return false;
        if (loaded_) SDL_DestroySurface(loaded_);
        loaded_ = rgba;
        return true;
    });
}

void MatteFrames::destroy() {
    stop();
    if (loaded_) SDL_DestroySurface(loaded_);
    loaded_ = nullptr;
    if (device_) {
        if (texture_) SDL_ReleaseGPUTexture(device_, texture_);
        if (transfer_) SDL_ReleaseGPUTransferBuffer(device_, transfer_);
    }
    texture_ = nullptr;
    transfer_ = nullptr;
    width_ = height_ = 0;
    done_count_ = 0;
}

bool MatteFrames::rescan() {
    std::vector<bool> done(done_.size(), false);
    std::int64_t count = 0;
    fs::file_time_type newest{};
    std::error_code ec;
    for (fs::directory_iterator it(dir_, ec), end; !ec && it != end; it.increment(ec)) {
        std::int64_t index = 0;
        if (!frame_of(it->path().filename().string(), &index)) continue;
        if (index < 0 || index >= static_cast<std::int64_t>(done.size())) continue;
        done[static_cast<std::size_t>(index)] = true;
        ++count;
        const fs::file_time_type written = it->last_write_time(ec);
        if (!ec && written > newest) newest = written;
    }
    const bool changed = done != done_;
    done_ = std::move(done);
    done_count_ = count;
    newest_ = newest;
    return changed;
}

bool MatteFrames::matted(std::int64_t frame) const {
    return frame >= 0 && frame < static_cast<std::int64_t>(done_.size()) &&
           done_[static_cast<std::size_t>(frame)];
}

bool MatteFrames::upload(SDL_GPUCommandBuffer* cmd) {
    SDL_Surface* rgba = loaded_;
    if (!rgba) return false;
    if (!texture_ || rgba->w != width_ || rgba->h != height_) {
        if (texture_) SDL_ReleaseGPUTexture(device_, texture_);
        if (transfer_) SDL_ReleaseGPUTransferBuffer(device_, transfer_);
        SDL_GPUTextureCreateInfo info = {};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = static_cast<Uint32>(rgba->w);
        info.height = static_cast<Uint32>(rgba->h);
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        texture_ = SDL_CreateGPUTexture(device_, &info);
        SDL_GPUTransferBufferCreateInfo tb = {};
        tb.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tb.size = static_cast<Uint32>(rgba->w) * static_cast<Uint32>(rgba->h) * 4;
        transfer_ = SDL_CreateGPUTransferBuffer(device_, &tb);
        if (!texture_ || !transfer_) {
            SDL_Log("cannot make the matte texture: %s", SDL_GetError());
            return false;
        }
        width_ = rgba->w;
        height_ = rgba->h;
    }

    auto* mapped = static_cast<std::uint8_t*>(SDL_MapGPUTransferBuffer(device_, transfer_, true));
    if (!mapped) return false;
    const std::size_t row = static_cast<std::size_t>(width_) * 4;
    for (int y = 0; y < height_; ++y)
        std::memcpy(mapped + y * row, static_cast<const std::uint8_t*>(rgba->pixels) + y * rgba->pitch,
                    row);
    SDL_UnmapGPUTransferBuffer(device_, transfer_);

    // Recorded into the frame's own command buffer, ahead of the pass that
    // samples it.
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src = {};
    src.transfer_buffer = transfer_;
    src.pixels_per_row = static_cast<Uint32>(width_);
    src.rows_per_layer = static_cast<Uint32>(height_);
    SDL_GPUTextureRegion dst = {};
    dst.texture = texture_;
    dst.w = static_cast<Uint32>(width_);
    dst.h = static_cast<Uint32>(height_);
    dst.d = 1;
    SDL_UploadToGPUTexture(copy, &src, &dst, /*cycle=*/true);
    SDL_EndGPUCopyPass(copy);
    return true;
}

}  // namespace ps::app

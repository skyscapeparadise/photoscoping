#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <functional>

#include "media/frame_worker.hpp"

namespace ps::app {

// Something the viewer shows a frame of -- the clip, a render of it, a matte
// PNG -- made on a worker thread and uploaded on the main one.
//
// The app asks every source the view needs for the same frame and shows it
// only once all of them have it, so footage and matte never disagree about
// which frame they are.
class FrameSource {
public:
    virtual ~FrameSource() = default;

    // Called on the worker when a frame is made. Set before the source is
    // opened; it is handed to the worker as it starts.
    void set_notify(std::function<void()> notify) { notify_ = std::move(notify); }

    // The frame this source's texture holds, or -1.
    std::int64_t shown() const { return shown_; }

    void request(std::int64_t index) {
        if (worker_.running()) worker_.request(index);
    }
    std::int64_t ready() const { return worker_.running() ? worker_.ready() : -1; }
    std::int64_t failed() const { return worker_.running() ? worker_.failed() : -1; }
    bool has(std::int64_t index) const { return shown_ == index || ready() == index; }

    // Uploads the ready frame and lets the worker go on. False when there is
    // none, or the upload failed.
    bool take(SDL_GPUCommandBuffer* cmd) {
        const std::int64_t index = ready();
        if (index < 0) return false;
        const bool ok = upload(cmd);
        worker_.release();
        if (ok) shown_ = index;
        return ok;
    }
    // Lets go of a ready frame that is neither of these: asked for, and then
    // overtaken by a seek.
    void drop_stale(std::int64_t keep, std::int64_t also_keep) {
        const std::int64_t index = ready();
        if (index >= 0 && index != keep && index != also_keep) worker_.release();
    }
    // Blocks until `index` is ready or failed. For screenshots.
    bool wait(std::int64_t index) { return shown_ == index || worker_.wait(index); }
    // The frame shown must be made again: what it was made from changed.
    void refresh() {
        worker_.invalidate();
        shown_ = -1;
    }

protected:
    // Runs on the main thread while the worker holds still.
    virtual bool upload(SDL_GPUCommandBuffer* cmd) = 0;

    // Derived classes stop the worker in their own destructor or destroy(),
    // before anything produce reads goes away -- by the time this base's
    // destructor runs, their members are already gone.
    void start(media::FrameWorker::Produce produce) {
        shown_ = -1;
        worker_.start(std::move(produce), notify_);
    }
    void stop() {
        worker_.stop();
        shown_ = -1;
    }

    media::FrameWorker worker_;
    std::function<void()> notify_;
    std::int64_t shown_ = -1;
};

}  // namespace ps::app

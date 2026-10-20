#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

namespace ps::media {

// Makes frames on a thread of its own, one at a time, as they are asked for.
//
// Decoding a frame of 4K footage, or reading a PNG of one, takes tens of
// milliseconds, and the main thread is the one that draws: done there, every
// seek and every scrub stalls the window. Here the work runs alongside, and
// the main thread picks up the result when it is there.
//
// What a frame *is* stays with the owner -- a decoder's current planes, a
// loaded image -- and `produce` is what makes it. The handoff keeps the two
// threads off it at once: once a frame is ready the worker holds still, so
// whatever produce made stays put until the main thread calls release().
class FrameWorker {
public:
    using Produce = std::function<bool(std::int64_t index)>;
    // Called on the worker thread each time a frame is made, or fails, so a
    // main loop asleep on its events can be woken to pick it up.
    using Notify = std::function<void()>;

    FrameWorker() = default;
    ~FrameWorker() { stop(); }
    FrameWorker(const FrameWorker&) = delete;
    FrameWorker& operator=(const FrameWorker&) = delete;

    void start(Produce produce, Notify notify = {});
    void stop();
    bool running() const { return thread_.joinable(); }

    // Asks for frame `index`. A later request replaces an earlier one that
    // has not started; one already being made is finished first, and comes
    // back stale.
    void request(std::int64_t index);
    // Makes the next request for `index` produce it afresh, even if it was
    // the last one made -- for when what produce reads has changed under it.
    void invalidate();

    // The frame made and waiting for the main thread, or -1.
    std::int64_t ready() const;
    // The frame produce last failed to make, or -1: past the end of a clip,
    // or a matte that stops short of it.
    std::int64_t failed() const;
    // Done with the ready frame; the worker may make the next.
    void release();

    // Blocks until `index` is ready or has failed, asking for it if need
    // be. For a caller that needs the frame now -- a screenshot -- and not
    // for the frame loop.
    bool wait(std::int64_t index);

private:
    void loop();

    Produce produce_;
    Notify notify_;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    bool stopping_ = false;
    std::int64_t requested_ = -1;
    std::int64_t made_ = -1;    // the last index produce was run for
    std::int64_t ready_ = -1;
    std::int64_t failed_ = -1;
};

}  // namespace ps::media

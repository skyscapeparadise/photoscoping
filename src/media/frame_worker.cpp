#include "media/frame_worker.hpp"

namespace ps::media {

void FrameWorker::start(Produce produce, Notify notify) {
    stop();
    produce_ = std::move(produce);
    notify_ = std::move(notify);
    stopping_ = false;
    requested_ = made_ = ready_ = failed_ = -1;
    thread_ = std::thread([this] { loop(); });
}

void FrameWorker::stop() {
    if (!thread_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    changed_.notify_all();
    thread_.join();
}

void FrameWorker::request(std::int64_t index) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (requested_ == index) return;
        requested_ = index;
    }
    changed_.notify_all();
}

void FrameWorker::invalidate() {
    std::lock_guard<std::mutex> lock(mutex_);
    made_ = -1;
    requested_ = -1;
}

std::int64_t FrameWorker::ready() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ready_;
}

std::int64_t FrameWorker::failed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return failed_;
}

void FrameWorker::release() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ready_ = -1;
    }
    changed_.notify_all();
}

bool FrameWorker::wait(std::int64_t index) {
    request(index);
    std::unique_lock<std::mutex> lock(mutex_);
    // Made and already handed over counts as done: there is nothing more
    // coming for it, and waiting would be forever.
    changed_.wait(lock, [&] {
        return stopping_ || ready_ == index || failed_ == index ||
               (made_ == index && ready_ < 0);
    });
    return ready_ == index;
}

void FrameWorker::loop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        // Idle while a frame waits to be picked up, and while there is
        // nothing new to make.
        changed_.wait(lock, [&] {
            return stopping_ || (ready_ < 0 && requested_ >= 0 && requested_ != made_);
        });
        if (stopping_) return;
        const std::int64_t index = requested_;
        lock.unlock();
        const bool ok = produce_(index);
        lock.lock();
        made_ = index;
        if (ok) {
            ready_ = index;
            if (failed_ == index) failed_ = -1;
        } else {
            failed_ = index;
        }
        lock.unlock();
        changed_.notify_all();
        if (notify_) notify_();
        lock.lock();
    }
}

}  // namespace ps::media

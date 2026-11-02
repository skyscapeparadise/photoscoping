#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace ps::app {

// The app's own matting process, kept running between single frames (this
// executable started again with --matte-serve): the models loaded and warm,
// so after the first, a frame asked for costs only its own matting.
//
// Frames go to it on stdin, one request a line; it answers on stdout, read
// here without ever blocking. It is the app's to stop: when the clip changes,
// when a full run wants the GPU, or after it has sat idle long enough that
// keeping its memory is no longer worth it.
class MatteServer {
public:
    MatteServer() = default;
    ~MatteServer();
    MatteServer(const MatteServer&) = delete;
    MatteServer& operator=(const MatteServer&) = delete;

    // Starts it for a clip, writing where --matte-clip would.
    bool start(const std::string& app, const std::string& models, const std::string& clip,
               const std::string& lut, const std::string& out, const std::string& name);
    void stop();
    bool running() const { return process_ != nullptr; }
    // Warm and listening.
    bool ready() const { return ready_; }

    // Asks for a frame; it is matted after those already asked for.
    void request(std::int64_t frame);
    bool asked(std::int64_t frame) const;
    // The frame being matted now and how many wait behind it; -1 for none.
    std::int64_t current() const { return queue_.empty() ? -1 : queue_.front(); }
    std::size_t waiting() const { return queue_.empty() ? 0 : queue_.size() - 1; }
    bool busy() const { return !queue_.empty(); }

    // Reads what it has said. The frames finished since the last call; and
    // `died` if it is gone -- crashed, or quit on its own.
    std::vector<std::int64_t> poll(bool* died);
    // Seconds since it last finished anything, or since it started.
    double idle_seconds() const;

private:
    void send(const std::string& line);

    SDL_Process* process_ = nullptr;
    SDL_IOStream* log_ = nullptr;
    std::string pending_;  // stdout read but not yet a whole line
    std::deque<std::int64_t> queue_;
    bool ready_ = false;
    Uint64 last_done_ns_ = 0;
};

}  // namespace ps::app

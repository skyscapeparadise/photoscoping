#pragma once

#include <SDL3/SDL.h>

#include <string>

namespace ps::app {

// A run of tools/matte_clip.py over the open clip, in the background.
//
// The matting stack is Python, so the app does not do the work; it starts
// the tool, lets it write each frame's matte as it finishes, and watches the
// directory. Stopping kills the tool, and starting again resumes it -- frames
// already written are not matted twice -- so closing the window mid-clip
// costs only the frame that was in progress.
//
// The tool lives in the source checkout, beside its Python environment, so
// this only works from a build of the checkout. available() says whether
// both were found.
class MatteJob {
public:
    MatteJob() = default;
    ~MatteJob();
    MatteJob(const MatteJob&) = delete;
    MatteJob& operator=(const MatteJob&) = delete;

    // Finds the tool, its Python and this executable. Call once.
    void locate();
    bool available() const { return !python_.empty(); }

    // `lut` may be empty. Output goes under `out`, where the viewer reads it;
    // the tool's own output goes to a log there.
    bool start(const std::string& clip, const std::string& lut, const std::string& out,
               const std::string& name);
    // Asks the tool to stop and waits for it.
    void stop();

    bool running() const { return process_ != nullptr; }
    // Checks on a running tool. True once, the call after it exits, with
    // `succeeded` saying whether it finished the clip.
    bool poll_finished(bool* succeeded);

private:
    std::string python_;
    std::string script_;
    std::string app_;
    SDL_Process* process_ = nullptr;
    SDL_IOStream* log_ = nullptr;
};

}  // namespace ps::app

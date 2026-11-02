#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>

namespace ps::app {

// A run of the native matting stack over the open clip, in the background.
//
// The run is this same executable, started again headless with --matte-clip:
// a process of its own, so the editor never shares a GPU device across
// threads with it, and a crash or a runaway in the matting cannot take the
// editor down. It writes each frame's matte as it finishes, and the app
// watches the directory. Stopping kills it, and starting again resumes it --
// frames already written are not matted twice -- so closing the window
// mid-clip costs only the frame that was in progress.
//
// The models are tools/export_matting_model.py's, looked for in a models/
// directory beside the executable or up from it. available() says whether
// they and the executable were found.
class MatteJob {
public:
    MatteJob() = default;
    ~MatteJob();
    MatteJob(const MatteJob&) = delete;
    MatteJob& operator=(const MatteJob&) = delete;

    // Finds this executable and the models. Call once.
    void locate();
    bool available() const { return !app_.empty() && !models_.empty(); }

    // What a run makes: the whole clip's mattes (--matte-clip), the patches
    // its re-matte boxes want (--rematte-clip), an export of it
    // (--export-clip), or its clean plate (--plate-clip). One at a time --
    // they all want the GPU.
    enum class Kind { kMatte, kRematte, kExport, kPlate, kLights };

    // `lut` may be empty. Output goes under `out`, where the viewer reads it;
    // the run's own output goes to a log there.
    // For an export, `out` is where the clip's renders are, and `export_to`
    // and `format` (an ExportFormat's id) say what to write where.
    // `range`, "A:B", limits a run to those frames -- one frame, matted for
    // a look at it.
    bool start(Kind kind, const std::string& clip, const std::string& lut, const std::string& out,
               const std::string& name, const std::string& export_to = {},
               const std::string& format = {}, const std::string& range = {});
    // How far an export has got, from the last progress line in its log.
    // False until it has written one.
    bool export_progress(std::int64_t* done, std::int64_t* total) const;
    const std::string& log_path() const { return log_path_; }
    Kind kind() const { return kind_; }
    // Stops the run and waits for it to be gone.
    void stop();

    bool running() const { return process_ != nullptr; }
    // Checks on a running run. True once, the call after it exits, with
    // `succeeded` saying whether it finished the clip.
    bool poll_finished(bool* succeeded);

private:
    std::string app_;
    std::string models_;
    SDL_Process* process_ = nullptr;
    Kind kind_ = Kind::kMatte;
    std::string log_path_;
    SDL_IOStream* log_ = nullptr;
};

}  // namespace ps::app

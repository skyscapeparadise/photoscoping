#include "app/matte_job.hpp"

#include <filesystem>
#include <vector>

namespace ps::app {

namespace {

namespace fs = std::filesystem;

bool present(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

}  // namespace

MatteJob::~MatteJob() { stop(); }

void MatteJob::locate() {
    const char* base = SDL_GetBasePath();
    if (!base) return;
    const fs::path root(base);

    // build/photoscoping when run bare; inside the .app, MacOS/ beside the
    // Resources directory SDL reports.
    for (const fs::path& candidate : {root / "photoscoping", root / ".." / "MacOS" / "photoscoping"}) {
        if (present(candidate)) {
            app_ = candidate.lexically_normal().string();
            break;
        }
    }
    // The checkout is a few levels above either: build/, or
    // build/photoscoping.app/Contents/Resources/.
    fs::path dir = root;
    for (int up = 0; up < 5 && !app_.empty(); ++up, dir = dir / "..") {
        const fs::path script = dir / "tools" / "matte_clip.py";
        const fs::path python = dir / ".venv" / "bin" / "python";
        if (present(script) && present(python)) {
            script_ = script.lexically_normal().string();
            python_ = python.lexically_normal().string();
            return;
        }
    }
}

bool MatteJob::start(const std::string& clip, const std::string& lut, const std::string& out,
                     const std::string& name) {
    if (!available() || running()) return false;
    std::error_code ec;
    fs::create_directories(out, ec);

    std::vector<std::string> args = {python_, script_, clip, "--out", out, "--app", app_};
    if (!lut.empty()) {
        args.push_back("--lut");
        args.push_back(lut);
    }
    std::vector<const char*> argv;
    for (const std::string& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);

    // Everything the tool says goes to a log beside its output rather than
    // into this process's terminal, where it would interleave with ours.
    const std::string log_path = (fs::path(out) / (name + "_matte.log")).string();
    log_ = SDL_IOFromFile(log_path.c_str(), "w");

    // Our environment, less Metal's validation layer. SDL switches that on
    // for this process when the GPU device is made in debug mode, a child
    // inherits it, and PyTorch's Metal kernels fail its checks -- the tool
    // dies on an assertion a few seconds in, though it runs cleanly from a
    // terminal.
    SDL_Environment* env = SDL_CreateEnvironment(/*populated=*/true);
    if (env) SDL_UnsetEnvironmentVariable(env, "MTL_DEBUG_LAYER");

    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
    if (env) SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, env);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    if (log_) {
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER,
                              SDL_PROCESS_STDIO_REDIRECT);
        SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_POINTER, log_);
        SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    }
    process_ = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (env) SDL_DestroyEnvironment(env);
    if (!process_) {
        SDL_Log("could not start %s: %s", script_.c_str(), SDL_GetError());
        if (log_) SDL_CloseIO(log_);
        log_ = nullptr;
        return false;
    }
    SDL_Log("matting %s into %s (log: %s)", clip.c_str(), out.c_str(), log_path.c_str());
    return true;
}

bool MatteJob::poll_finished(bool* succeeded) {
    if (!process_) return false;
    int code = -1;
    if (!SDL_WaitProcess(process_, /*block=*/false, &code)) return false;
    SDL_DestroyProcess(process_);
    process_ = nullptr;
    if (log_) SDL_CloseIO(log_);
    log_ = nullptr;
    if (succeeded) *succeeded = code == 0;
    return true;
}

void MatteJob::stop() {
    if (!process_) return;
    // A plain terminate is enough: the tool's own children are the frame
    // stream and two encoders, and its exit closes the pipes that feed them,
    // which ends them too. The frames written so far stay, for a resume.
    SDL_KillProcess(process_, /*force=*/false);
    SDL_WaitProcess(process_, /*block=*/true, nullptr);
    SDL_DestroyProcess(process_);
    process_ = nullptr;
    if (log_) SDL_CloseIO(log_);
    log_ = nullptr;
}

}  // namespace ps::app

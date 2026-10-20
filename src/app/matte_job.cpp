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
    // models/ in Resources, or in the checkout a few levels above build/ or
    // build/photoscoping.app/Contents/Resources/.
    fs::path dir = root;
    for (int up = 0; up < 5; ++up, dir = dir / "..") {
        const fs::path models = dir / "models";
        if (present(models / "birefnet_hr_matting_2048x2048_fp16.onnx") &&
            present(models / "birefnet_hr_matting_2048x1152_fp16.onnx")) {
            models_ = models.lexically_normal().string();
            return;
        }
    }
}

bool MatteJob::start(const std::string& clip, const std::string& lut, const std::string& out,
                     const std::string& name) {
    if (!available() || running()) return false;
    std::error_code ec;
    fs::create_directories(out, ec);

    std::vector<std::string> args = {app_, clip, "--matte-clip", out, "--models", models_};
    if (!lut.empty()) {
        args.push_back("--lut");
        args.push_back(lut);
    }
    std::vector<const char*> argv;
    for (const std::string& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);

    // Everything the run says goes to a log beside its output rather than
    // into this process's terminal, where it would interleave with ours.
    const std::string log_path = (fs::path(out) / (name + "_matte.log")).string();
    log_ = SDL_IOFromFile(log_path.c_str(), "w");

    // Our environment, less Metal's validation layer. SDL switches that on
    // for this process when a debug build makes its GPU device, a child
    // inherits it, and Core ML's Metal kernels fail its checks -- PyTorch's
    // did, a few seconds in, when the run was Python.
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
        SDL_Log("could not start the matting run: %s", SDL_GetError());
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
    // Killed outright. A terminate would not do: the run has SDL, which turns
    // the signal into a quit event its loop never reads, and waiting on it
    // would hold the editor until the whole clip was done. Nothing is lost --
    // every frame is renamed into place whole, and the ones written so far
    // stay for a resume.
    SDL_KillProcess(process_, /*force=*/true);
    SDL_WaitProcess(process_, /*block=*/true, nullptr);
    SDL_DestroyProcess(process_);
    process_ = nullptr;
    if (log_) SDL_CloseIO(log_);
    log_ = nullptr;
}

}  // namespace ps::app

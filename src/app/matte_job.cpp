#include "app/matte_job.hpp"

#include <cstdio>
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

bool MatteJob::start(Kind kind, const std::string& clip, const std::string& lut, const std::string& out,
                     const std::string& name, const std::string& export_to,
                     const std::string& format) {
    if (!available() || running()) return false;
    std::error_code ec;
    fs::create_directories(out, ec);

    const bool rematte = kind == Kind::kRematte;
    std::vector<std::string> args = {app_, clip};
    if (kind == Kind::kExport) {
        args.insert(args.end(), {"--mattes", out, "--export-clip", export_to, "--export-format", format});
    } else if (kind == Kind::kPlate) {
        args.insert(args.end(), {"--plate-clip", out});
    } else {
        args.insert(args.end(), {rematte ? "--rematte-clip" : "--matte-clip", out});
    }
    args.insert(args.end(), {"--models", models_});
    if (!lut.empty()) {
        args.push_back("--lut");
        args.push_back(lut);
    }
    std::vector<const char*> argv;
    for (const std::string& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);

    // Everything the run says goes to a log beside its output rather than
    // into this process's terminal, where it would interleave with ours.
    const char* suffix = kind == Kind::kExport  ? "_export.log"
                         : kind == Kind::kPlate ? "_plate.log"
                         : rematte              ? "_rematte.log"
                                                : "_matte.log";
    const std::string log_path = (fs::path(out) / (name + suffix)).string();
    log_path_ = log_path;
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
    kind_ = kind;
    SDL_Log("%s %s into %s (log: %s)",
            kind == Kind::kExport  ? "exporting"
            : kind == Kind::kPlate ? "making the plate for"
            : rematte              ? "re-matting"
                                   : "matting",
            clip.c_str(),
            kind == Kind::kExport ? export_to.c_str() : out.c_str(), log_path.c_str());
    return true;
}

bool MatteJob::export_progress(std::int64_t* done, std::int64_t* total) const {
    // The log's tail, back to the last "  n/m  frame ..." line.
    std::FILE* f = std::fopen(log_path_.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    const long from = size > 4096 ? size - 4096 : 0;
    std::fseek(f, from, SEEK_SET);
    std::string tail(static_cast<std::size_t>(size - from), '\0');
    tail.resize(std::fread(tail.data(), 1, tail.size(), f));
    std::fclose(f);
    bool found = false;
    for (std::size_t at = 0; at < tail.size();) {
        std::size_t end = tail.find('\n', at);
        if (end == std::string::npos) end = tail.size();
        long long d = 0, t = 0;
        if (std::sscanf(tail.c_str() + at, "  %lld/%lld  frame", &d, &t) == 2) {
            *done = d;
            *total = t;
            found = true;
        }
        at = end + 1;
    }
    return found;
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

#include "app/matte_server.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace ps::app {

MatteServer::~MatteServer() { stop(); }

bool MatteServer::start(const std::string& app, const std::string& models, const std::string& clip,
                        const std::string& lut, const std::string& out, const std::string& name) {
    stop();
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(out, ec);
    std::vector<std::string> args = {app, clip, "--matte-serve", out, "--models", models};
    if (!lut.empty()) args.insert(args.end(), {"--lut", lut});
    std::vector<const char*> argv;
    for (const std::string& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);

    const std::string log_path = (fs::path(out) / (name + "_matte_serve.log")).string();
    log_ = SDL_IOFromFile(log_path.c_str(), "w");
    // As MatteJob does: our environment less Metal's validation layer.
    SDL_Environment* env = SDL_CreateEnvironment(/*populated=*/true);
    if (env) SDL_UnsetEnvironmentVariable(env, "MTL_DEBUG_LAYER");

    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
    if (env) SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, env);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    if (log_) {
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_REDIRECT);
        SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_POINTER, log_);
    }
    process_ = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (env) SDL_DestroyEnvironment(env);
    if (!process_) {
        SDL_Log("could not start the matting process: %s", SDL_GetError());
        if (log_) SDL_CloseIO(log_);
        log_ = nullptr;
        return false;
    }
    ready_ = false;
    pending_.clear();
    queue_.clear();
    last_done_ns_ = SDL_GetTicksNS();
    SDL_Log("matting process started for %s (log: %s)", clip.c_str(), log_path.c_str());
    return true;
}

void MatteServer::stop() {
    if (!process_) return;
    // Asked to go, then made to: a frame in progress is not waited for --
    // its matte is written whole or not at all.
    send("quit");
    SDL_KillProcess(process_, /*force=*/true);
    SDL_WaitProcess(process_, /*block=*/true, nullptr);
    SDL_DestroyProcess(process_);
    process_ = nullptr;
    if (log_) SDL_CloseIO(log_);
    log_ = nullptr;
    ready_ = false;
    queue_.clear();
    pending_.clear();
}

void MatteServer::send(const std::string& line) {
    if (!process_) return;
    SDL_IOStream* in = SDL_GetProcessInput(process_);
    if (!in) return;
    const std::string text = line + "\n";
    SDL_WriteIO(in, text.data(), text.size());
    SDL_FlushIO(in);
}

void MatteServer::request(std::int64_t frame) {
    if (!process_ || asked(frame)) return;
    queue_.push_back(frame);
    send("matte " + std::to_string(frame));
}

bool MatteServer::asked(std::int64_t frame) const {
    return std::find(queue_.begin(), queue_.end(), frame) != queue_.end();
}

double MatteServer::idle_seconds() const {
    return busy() ? 0.0 : static_cast<double>(SDL_GetTicksNS() - last_done_ns_) / 1e9;
}

std::vector<std::int64_t> MatteServer::poll(bool* died) {
    std::vector<std::int64_t> finished;
    if (died) *died = false;
    if (!process_) return finished;
    if (SDL_IOStream* out = SDL_GetProcessOutput(process_)) {
        char buffer[512];
        for (;;) {
            const std::size_t got = SDL_ReadIO(out, buffer, sizeof buffer);
            if (got == 0) break;
            pending_.append(buffer, got);
        }
    }
    for (std::size_t end; (end = pending_.find('\n')) != std::string::npos;) {
        const std::string line = pending_.substr(0, end);
        pending_.erase(0, end + 1);
        long long frame = -1;
        if (line == "ready") {
            ready_ = true;
        } else if (std::sscanf(line.c_str(), "done %lld", &frame) == 1 ||
                   std::sscanf(line.c_str(), "error %lld", &frame) == 1) {
            if (line.rfind("error", 0) == 0) SDL_Log("matting frame %lld failed: %s", frame, line.c_str());
            const auto it = std::find(queue_.begin(), queue_.end(), static_cast<std::int64_t>(frame));
            if (it != queue_.end()) queue_.erase(it);
            finished.push_back(frame);
            last_done_ns_ = SDL_GetTicksNS();
        }
    }
    int code = 0;
    if (SDL_WaitProcess(process_, /*block=*/false, &code)) {
        SDL_Log("the matting process ended (%d)", code);
        SDL_DestroyProcess(process_);
        process_ = nullptr;
        if (log_) SDL_CloseIO(log_);
        log_ = nullptr;
        ready_ = false;
        queue_.clear();
        if (died) *died = true;
    }
    return finished;
}

}  // namespace ps::app

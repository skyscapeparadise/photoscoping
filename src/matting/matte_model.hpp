#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Ort {
struct Env;
struct Session;
}  // namespace Ort

namespace ps::matting {

// The matting model, run natively through ONNX Runtime.
//
// On the Mac it runs on the GPU through Core ML. tools/onnx_spike.py measured
// the choices: the GPU in half precision is the fastest (2.5 s a frame at
// 2048, against PyTorch's 3.8), and leaving the Neural Engine out of it keeps
// the compiled model loading in half a second instead of two minutes, at no
// cost in speed. Core ML compiles a model the first time it sees it -- about
// seven minutes for this one -- and keeps the result in `cache_dir`, so that
// is paid once per machine and model, not per run.
//
// Elsewhere, for now, the CPU.
class MatteModel {
public:
    enum class Backend { kGpu, kCpu };

    MatteModel();
    ~MatteModel();
    MatteModel(const MatteModel&) = delete;
    MatteModel& operator=(const MatteModel&) = delete;

    // `path` is an ONNX export of the model (tools/onnx_spike.py export, then
    // fp16) taking a 1x3xSxS image and giving a 1x1xSxS alpha. False with a
    // message in `error` if it cannot be loaded.
    bool load(const std::string& path, Backend backend, const std::string& cache_dir,
              std::string* error);
    bool loaded() const { return session_ != nullptr; }
    // S, read from the model's input.
    int size() const { return size_; }

    // `input` is model_input()'s output; the result is size x size alpha,
    // 0..1. Empty with a message in `error` on failure.
    std::vector<float> run(const std::vector<float>& input, std::string* error);

private:
    std::unique_ptr<Ort::Env> env_;
    std::unique_ptr<Ort::Session> session_;
    int size_ = 0;
};

// Where Core ML's compiled models are kept: the user's caches, which the
// system may clear, which only costs a recompile.
std::string default_cache_dir();

}  // namespace ps::matting

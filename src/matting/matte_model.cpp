#include "matting/matte_model.hpp"

#include <onnxruntime_cxx_api.h>

#include <cstdlib>
#include <filesystem>

namespace ps::matting {

MatteModel::MatteModel() = default;
MatteModel::~MatteModel() = default;

bool MatteModel::load(const std::string& path, Backend backend, const std::string& cache_dir,
                      std::string* error) {
    session_.reset();
    try {
        if (!env_) env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_ERROR, "photoscoping");
        Ort::SessionOptions options;
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
#if defined(__APPLE__)
        if (backend == Backend::kGpu) {
            std::error_code ec;
            std::filesystem::create_directories(cache_dir, ec);
            options.AppendExecutionProvider("CoreML", {{"ModelFormat", "MLProgram"},
                                                       {"MLComputeUnits", "CPUAndGPU"},
                                                       {"ModelCacheDirectory", cache_dir}});
        }
#else
        (void)backend;
        (void)cache_dir;
#endif
        session_ = std::make_unique<Ort::Session>(*env_, path.c_str(), options);
        const auto shape =
            session_->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
        height_ = shape.size() == 4 ? static_cast<int>(shape[2]) : 0;
        width_ = shape.size() == 4 ? static_cast<int>(shape[3]) : 0;
        if (width_ <= 0 || height_ <= 0) {
            if (error) *error = "the model's input is not one fixed image size";
            session_.reset();
            return false;
        }
        return true;
    } catch (const Ort::Exception& e) {
        if (error) *error = e.what();
        session_.reset();
        return false;
    }
}

std::vector<float> MatteModel::run(const std::vector<float>& input, std::string* error) {
    if (!session_) {
        if (error) *error = "no model loaded";
        return {};
    }
    const std::size_t plane = static_cast<std::size_t>(width_) * height_;
    if (input.size() != plane * 3) {
        if (error) *error = "the input is not the model's size";
        return {};
    }
    try {
        const Ort::MemoryInfo memory =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        const int64_t shape[4] = {1, 3, height_, width_};
        // The input is only read; ONNX Runtime's API asks for it unconst.
        Ort::Value tensor = Ort::Value::CreateTensor<float>(
            memory, const_cast<float*>(input.data()), input.size(), shape, 4);
        const char* inputs[] = {"image"};
        const char* outputs[] = {"alpha"};
        auto result = session_->Run(Ort::RunOptions{nullptr}, inputs, &tensor, 1, outputs, 1);
        const float* alpha = result[0].GetTensorData<float>();
        return std::vector<float>(alpha, alpha + plane);
    } catch (const Ort::Exception& e) {
        if (error) *error = e.what();
        return {};
    }
}

std::string default_cache_dir() {
#if defined(__APPLE__)
    if (const char* home = std::getenv("HOME"))
        return (std::filesystem::path(home) / "Library" / "Caches" /
                "com.rainmultimedia.photoscoping" / "coreml")
            .string();
#endif
    return (std::filesystem::temp_directory_path() / "photoscoping-models").string();
}

}  // namespace ps::matting

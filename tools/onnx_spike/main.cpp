// The C++ side of tools/onnx_spike.py: the exported matting model through
// ONNX Runtime from native code, on the same preprocessed frames, so the two
// can be compared for speed and for the alpha they produce.
//
// Not part of the app. Built on its own against a prebuilt ONNX Runtime:
//
//   cmake -S tools/onnx_spike -B build/onnx_spike -DORT_ROOT=/path/to/onnxruntime
//   cmake --build build/onnx_spike
//   build/onnx_spike/onnx_spike renders/onnx_spike cpu [MODEL LABEL]
//
// Reads MODEL (default birefnet_hr_matting_2048.onnx) from <dir> and every
// <dir>/input_*.f32, and writes <dir>/cpp_<label>_<frame>.f32 beside them for
// `onnx_spike.py compare`. The label defaults to the provider.

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int64_t kSize = 2048;

double seconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

std::vector<float> read_floats(const fs::path& path, std::size_t count) {
    std::vector<float> data(count);
    std::ifstream in(path, std::ios::binary);
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(count * sizeof(float)));
    if (!in) data.clear();
    return data;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: onnx_spike DIR cpu|coreml|coreml-gpu [MODEL LABEL]\n");
        return 1;
    }
    const fs::path dir = argv[1];
    const std::string provider = argv[2];
    const std::string model = argc > 3 ? argv[3] : "birefnet_hr_matting_2048.onnx";
    const std::string label = argc > 4 ? argv[4] : provider;

    Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "onnx_spike");
    Ort::SessionOptions options;
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    // coreml lets Core ML use the Neural Engine as well; coreml-gpu keeps it
    // to the CPU and GPU, which compiles far faster for a model this large.
    if (provider == "coreml" || provider == "coreml-gpu") {
        // Compiled once into the cache and loaded from it after: Core ML's
        // compile of this graph is minutes, not something to pay per launch.
        const std::string cache = (dir / ("coreml_cache_" + label)).string();
        fs::create_directories(cache);
        options.AppendExecutionProvider("CoreML", {{"ModelFormat", "MLProgram"},
                                                   {"MLComputeUnits", provider == "coreml"
                                                                          ? "ALL"
                                                                          : "CPUAndGPU"},
                                                   {"ModelCacheDirectory", cache}});
    }

    auto t0 = std::chrono::steady_clock::now();
    Ort::Session session(env, (dir / model).c_str(), options);
    std::printf("%s: session in %.1f s\n", provider.c_str(), seconds_since(t0));

    std::vector<fs::path> inputs;
    for (const auto& entry : fs::directory_iterator(dir)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("input_", 0) == 0 && entry.path().extension() == ".f32")
            inputs.push_back(entry.path());
    }
    std::sort(inputs.begin(), inputs.end());
    if (inputs.empty()) {
        std::fprintf(stderr, "no input_*.f32 in %s\n", dir.c_str());
        return 1;
    }

    const Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    const std::array<int64_t, 4> shape = {1, 3, kSize, kSize};
    const char* input_names[] = {"image"};
    const char* output_names[] = {"alpha"};

    auto run = [&](std::vector<float>& image) {
        Ort::Value tensor = Ort::Value::CreateTensor<float>(memory, image.data(), image.size(),
                                                            shape.data(), shape.size());
        return session.Run(Ort::RunOptions{nullptr}, input_names, &tensor, 1, output_names, 1);
    };

    // The first run allocates and, on some providers, finishes compiling;
    // it is reported on its own and left out of the average.
    std::vector<float> first = read_floats(inputs.front(), 3 * kSize * kSize);
    t0 = std::chrono::steady_clock::now();
    run(first);
    std::printf("%s: first run %.2f s\n", provider.c_str(), seconds_since(t0));

    double total = 0.0;
    for (const fs::path& path : inputs) {
        std::vector<float> image = read_floats(path, 3 * kSize * kSize);
        if (image.empty()) {
            std::fprintf(stderr, "cannot read %s\n", path.c_str());
            return 1;
        }
        t0 = std::chrono::steady_clock::now();
        auto outputs = run(image);
        const double s = seconds_since(t0);
        total += s;
        const float* alpha = outputs[0].GetTensorData<float>();
        const std::string frame = path.stem().string().substr(6);
        std::ofstream out(dir / ("cpp_" + label + "_" + frame + ".f32"), std::ios::binary);
        out.write(reinterpret_cast<const char*>(alpha),
                  static_cast<std::streamsize>(kSize * kSize * sizeof(float)));
        std::printf("%s: frame %s %.2f s\n", provider.c_str(), frame.c_str(), s);
    }
    std::printf("%s: %.2f s/frame over %zu frames\n", provider.c_str(), total / inputs.size(),
                inputs.size());
    return 0;
}

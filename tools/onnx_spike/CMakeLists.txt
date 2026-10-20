# The C++ half of the ONNX test (tools/onnx_spike.py). Standalone: not part of
# photoscoping's build, and needs a prebuilt ONNX Runtime at ORT_ROOT.
cmake_minimum_required(VERSION 3.20)
project(onnx_spike LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(ORT_ROOT "" CACHE PATH "unpacked ONNX Runtime release")
add_executable(onnx_spike main.cpp)
target_include_directories(onnx_spike PRIVATE "${ORT_ROOT}/include")
target_link_libraries(onnx_spike PRIVATE "${ORT_ROOT}/lib/libonnxruntime.dylib")
set_target_properties(onnx_spike PROPERTIES BUILD_RPATH "${ORT_ROOT}/lib")

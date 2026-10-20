cmake_minimum_required(VERSION 3.20)
project(photoscoping VERSION 0.1.0 LANGUAGES CXX)

# The run-loop hook that keeps the window drawing under AppKit's own loops is
# Cocoa, so the Objective-C++ compiler is needed for that one file.
if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# One source of truth for the version: the project() line above.
add_compile_definitions(PHOTOSCOPING_VERSION="${PROJECT_VERSION}")

# Default to an optimized build. With CMAKE_BUILD_TYPE unset CMake passes no
# -O flag at all, so everything -- including anything packaged for release --
# would be compiled at -O0.
# Override for development with: cmake -B build -DCMAKE_BUILD_TYPE=Debug
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING
        "Build type (Debug, Release, RelWithDebInfo, MinSizeRel)" FORCE)
endif()
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# Point find_package at a vendored prefix, overriding any cached value.
#
# CMAKE_PREFIX_PATH alone is not enough: once find_package has resolved a
# package it caches <Pkg>_DIR, and a later prefix-path change is ignored. An
# already-configured build/ would keep linking Homebrew's SDL3 after the
# vendored one appeared and load two copies of SDL3 at runtime. Forcing the
# cache entry makes the resolution a function of what is on disk, not of
# configure order.
function(photoscoping_prefer_vendored pkg subdir)
    set(vendored "${CMAKE_CURRENT_SOURCE_DIR}/vendor/${subdir}/lib/cmake/${pkg}")
    if(EXISTS "${vendored}/${pkg}Config.cmake")
        message(STATUS "Using vendored ${pkg}: ${vendored}")
        set(${pkg}_DIR "${vendored}" CACHE PATH "${pkg} config dir" FORCE)
    else()
        message(WARNING
            "Vendored ${pkg} not found at ${vendored}. Falling back to the system "
            "one. Run vendor/build_*.sh before packaging a release.")
    endif()
endfunction()

# 3.4.0 minimum: the renderer is built on SDL_GPU's swapchain-composition API
# (extended linear for EDR). Vendored and version-pinned (vendor/build_sdl3.sh)
# rather than taken from Homebrew, which tracks whatever is current and bakes
# an absolute /opt/homebrew path into the binary that cannot ship.
photoscoping_prefer_vendored(SDL3 sdl3)
find_package(SDL3 3.4.0 REQUIRED)

# Vendored SDL3_ttf + HarfBuzz (vendor/build_text_stack.sh). Homebrew's
# harfbuzz links glib and graphite2, ~2.2MB for nothing the widget set uses.
photoscoping_prefer_vendored(SDL3_ttf textstack)
find_package(SDL3_ttf REQUIRED)

# Vendored SDL3_image (vendor/build_sdl3_image.sh): SVG for the widget icons,
# without the ~12MB of AVIF/JXL/TIFF/WebP decoders Homebrew's build carries.
photoscoping_prefer_vendored(SDL3_image sdl3_image)
find_package(SDL3_image REQUIRED)

# Vendored LGPL decode-only FFmpeg (vendor/build_ffmpeg.sh). Homebrew's is
# --enable-gpl --enable-version3 and must never be shipped: bundling it would
# relicense photoscoping as GPLv3. The vendor script asserts LGPL per library.
set(PHOTOSCOPING_VENDORED_FFMPEG "${CMAKE_CURRENT_SOURCE_DIR}/vendor/ffmpeg")
if(EXISTS "${PHOTOSCOPING_VENDORED_FFMPEG}/lib/pkgconfig/libavcodec.pc")
    message(STATUS "Using vendored LGPL FFmpeg: ${PHOTOSCOPING_VENDORED_FFMPEG}")
    set(ENV{PKG_CONFIG_PATH} "${PHOTOSCOPING_VENDORED_FFMPEG}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
else()
    message(WARNING
        "Vendored FFmpeg not found. Falling back to the system one, which is "
        "almost certainly GPL and must not be shipped. Run vendor/build_ffmpeg.sh.")
endif()
find_package(PkgConfig REQUIRED)
pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil)
pkg_check_modules(SWSCALE REQUIRED IMPORTED_TARGET libswscale)
pkg_check_modules(SWRESAMPLE REQUIRED IMPORTED_TARGET libswresample)

# Vendored ONNX Runtime (vendor/fetch_onnxruntime.sh): the matting model, run
# natively -- Core ML on the Mac. A pinned official release, not a system one.
set(PHOTOSCOPING_VENDORED_ORT "${CMAKE_CURRENT_SOURCE_DIR}/vendor/onnxruntime")
if(NOT EXISTS "${PHOTOSCOPING_VENDORED_ORT}/include/onnxruntime_cxx_api.h")
    message(FATAL_ERROR "Vendored ONNX Runtime missing. Run vendor/fetch_onnxruntime.sh")
endif()
add_library(onnxruntime SHARED IMPORTED)
set_target_properties(onnxruntime PROPERTIES
    IMPORTED_LOCATION "${PHOTOSCOPING_VENDORED_ORT}/lib/libonnxruntime.1.31.0.dylib"
    IMPORTED_SONAME "@rpath/libonnxruntime.1.dylib"
    INTERFACE_INCLUDE_DIRECTORIES "${PHOTOSCOPING_VENDORED_ORT}/include")

# Color math and LUTs: no GPU, no FFmpeg, so the headless tests can link it
# and check the arithmetic the shaders mirror.
add_library(photoscoping_color STATIC
    src/color/color_space.cpp
    src/color/cube_lut.cpp
    src/color/ycbcr.cpp
)
target_include_directories(photoscoping_color PUBLIC src)

# Decoding: FFmpeg, no GPU.
add_library(photoscoping_media STATIC
    src/media/video_decoder.cpp
    src/media/frame_worker.cpp
)
target_include_directories(photoscoping_media PUBLIC src)
target_link_libraries(photoscoping_media PUBLIC
    photoscoping_color
    SDL3::SDL3
    PkgConfig::AVCODEC
    PkgConfig::AVFORMAT
    PkgConfig::AVUTIL
)

# The matting stack, natively: preparing a frame for the model and running it.
# No GPU of SDL's and no window, so it can be tested headless.
add_library(photoscoping_matting STATIC
    src/matting/preprocess.cpp
    src/matting/matte_model.cpp
    src/matting/image_ops.cpp
    src/matting/stack.cpp
    src/matting/closed_form.cpp
    src/matting/foreground.cpp
    src/matting/png16.cpp
)
target_include_directories(photoscoping_matting PUBLIC src)
# zlib for the 16-bit PNG writer: the system's on macOS and Linux. Windows
# has none, and will need it vendored when that port comes. On macOS it is
# linked by name: FindZLIB's target adds the SDK's usr/include as an explicit
# include path, ahead of libc++'s own headers, which breaks <cstddef>.
target_link_libraries(photoscoping_matting PUBLIC onnxruntime)
if(APPLE)
    target_link_libraries(photoscoping_matting PUBLIC z)
else()
    find_package(ZLIB REQUIRED)
    target_link_libraries(photoscoping_matting PUBLIC ZLIB::ZLIB)
endif()

# Shaders: GLSL is the source of truth, compiled to SPIR-V (glslc) and then to
# MSL (spirv-cross) into a generated header, the same pipeline as understory.
# Generated into the build tree so nothing generated lives in src/. glslc and
# spirv-cross are build tools, not shipped libraries, so they are required
# rather than vendored.
set(PHOTOSCOPING_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/gpu/shaders_generated.hpp")
file(GLOB PHOTOSCOPING_SHADER_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/shaders/*")
add_custom_command(
    OUTPUT "${PHOTOSCOPING_SHADER_HEADER}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated/gpu"
    COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/tools/build_shaders.py"
                    "${PHOTOSCOPING_SHADER_HEADER}"
    DEPENDS ${PHOTOSCOPING_SHADER_SOURCES}
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/build_shaders.py"
    COMMENT "compiling shaders (glsl -> spir-v -> msl)"
    VERBATIM
)
add_custom_target(photoscoping_shaders DEPENDS "${PHOTOSCOPING_SHADER_HEADER}")

# The GPU backing, as a library rather than loose sources compiled into the
# executable, so the widget set can join it and be tested without a window.
add_library(photoscoping_ui STATIC
    src/gpu/gpu_context.cpp
    src/gpu/pipeline.cpp
    src/gpu/ingest_pass.cpp
    src/gpu/display_pass.cpp
    src/gpu/nebula_pass.cpp
    src/gpu/draw_list.cpp
    src/gpu/texture.cpp
    src/gpu/icon_atlas.cpp
    src/ui/node.cpp
    src/ui/widgets.cpp
    src/ui/font.cpp
    src/ui/text.cpp
    src/ui/layout.cpp
    src/editor/tool_strip.cpp
    src/editor/view_switch.cpp
    src/editor/timeline.cpp
    src/editor/inspector.cpp
    src/editor/editor_window.cpp
    src/editor/viewer.cpp
)
add_dependencies(photoscoping_ui photoscoping_shaders)
target_include_directories(photoscoping_ui PUBLIC src "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(photoscoping_ui PUBLIC
    photoscoping_color
    photoscoping_media
    SDL3::SDL3
    SDL3_ttf::SDL3_ttf
    SDL3_image::SDL3_image
)

add_executable(photoscoping src/main.cpp src/app/editor_app.cpp src/app/lockstep_stream.cpp
                            src/app/matte_frames.cpp src/app/matte_job.cpp
                            src/app/clip_frames.cpp)
target_link_libraries(photoscoping PRIVATE photoscoping_ui photoscoping_matting)

if(APPLE)
    target_sources(photoscoping PRIVATE src/app/tracking_ticker_mac.mm src/app/file_menu_mac.mm)
    set_source_files_properties(src/app/tracking_ticker_mac.mm src/app/file_menu_mac.mm
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(photoscoping PRIVATE "-framework Cocoa")
endif()

enable_testing()

add_executable(color_tests tests/test_color.cpp)
target_link_libraries(color_tests PRIVATE photoscoping_color)
add_test(NAME color COMMAND color_tests)

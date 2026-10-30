cmake_minimum_required(VERSION 3.25)
project(MeerCut VERSION 1.0.0 LANGUAGES CXX)
# The version users see (About box, file properties, package names); the numeric version above goes into resources.
# A pre-release adds a suffix at configure time (-DMEERCUT_VERSION_SUFFIX=-rc2); a release has none. (A normal variable:
# it also hides the MEERCUT_VERSION_LABEL cache entry that build trees configured for 1.0.0-rc1 still hold.)
set(MEERCUT_VERSION_SUFFIX "" CACHE STRING "Pre-release suffix of the version label, e.g. -rc2 (empty for a release)")
set(MEERCUT_VERSION_LABEL "${PROJECT_VERSION}${MEERCUT_VERSION_SUFFIX}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(FA_BUILD_TESTS "Build unit and end-to-end tests" ON)
option(FA_BUILD_GUI "Build the Qt desktop editor (fa-editor)" ON)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

# Everything a target needs (DLLs from vcpkg are copied next to executables by the vcpkg toolchain).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

find_package(PkgConfig QUIET)
find_package(FFMPEG REQUIRED)            # vcpkg's FindFFMPEG wrapper
find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenCV CONFIG REQUIRED COMPONENTS core imgproc video calib3d features2d)   # M5: tracking, stabilisation, optical flow (Apache-2.0)
# M7 local AI: ncnn (BSD-3-Clause, Vulkan) for Real-ESRGAN / RIFE, whisper.cpp + ggml (MIT, Vulkan) for speech,
# ONNX Runtime (MIT; the DirectML build of the NuGet package, using Windows' own DirectML.dll) for RVM / BiRefNet / YuNet.
find_package(ZLIB REQUIRED)
find_package(ncnn CONFIG REQUIRED)
find_package(whisper CONFIG REQUIRED)
set(FA_ONNXRUNTIME_DIR "C:/dev/third_party/onnxruntime-directml-1.24.4" CACHE PATH
    "ONNX Runtime from the NuGet package Microsoft.ML.OnnxRuntime.DirectML 1.24.4 (tools/fetch_m7_assets.py)")
set(_ort_bin "${FA_ONNXRUNTIME_DIR}/runtimes/win-x64/native")
if(NOT EXISTS "${_ort_bin}/onnxruntime.dll")
  message(FATAL_ERROR "ONNX Runtime not found in ${FA_ONNXRUNTIME_DIR}: run tools/fetch_m7_assets.py (BUILD.md)")
endif()
add_library(onnxruntime SHARED IMPORTED GLOBAL)
set_target_properties(onnxruntime PROPERTIES
  IMPORTED_LOCATION "${_ort_bin}/onnxruntime.dll"
  IMPORTED_IMPLIB "${_ort_bin}/onnxruntime.lib"
  INTERFACE_INCLUDE_DIRECTORIES "${FA_ONNXRUNTIME_DIR}/build/native/include")
file(COPY "${_ort_bin}/onnxruntime.dll" "${_ort_bin}/onnxruntime_providers_shared.dll"
     DESTINATION "${CMAKE_BINARY_DIR}/bin")   # next to every executable (vcpkg copies only its own DLLs)
set(FA_MODELS_DIR "C:/dev/fa-models" CACHE PATH "Installed AI models used by the M7 tests (<dir>/<model id>/...)")
if(FA_BUILD_GUI)
  find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Test)
endif()

add_subdirectory(src)
add_subdirectory(apps)

if(FA_BUILD_TESTS)
  enable_testing()
  find_package(GTest CONFIG REQUIRED)
  add_subdirectory(tests)
endif()

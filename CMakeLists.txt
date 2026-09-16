cmake_minimum_required(VERSION 3.22)

project(skai-edge-native VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET gstreamer-1.0)
pkg_check_modules(GST_VIDEO_APP REQUIRED IMPORTED_TARGET
    gstreamer-app-1.0 gstreamer-video-1.0 gstreamer-rtsp-1.0)
find_package(OpenCV REQUIRED COMPONENTS core imgproc)
find_package(Boost 1.74 REQUIRED COMPONENTS system)

add_library(skai-core INTERFACE)
target_include_directories(skai-core INTERFACE include)
target_link_libraries(skai-core INTERFACE Threads::Threads)

add_library(skai-events src/events.cpp)
target_include_directories(skai-events PUBLIC include)

add_library(skai-tensor-layout src/inference/tensor_layout.cpp)
target_include_directories(skai-tensor-layout PUBLIC include)

add_library(skai-preprocess-reference src/inference/preprocess.cpp)
target_include_directories(skai-preprocess-reference PUBLIC include)

add_library(skai-yolo-postprocess src/inference/yolo_postprocess.cpp)
target_include_directories(skai-yolo-postprocess PUBLIC include)
target_link_libraries(skai-yolo-postprocess PUBLIC skai-preprocess-reference)

add_library(skai-annotator src/video/annotator.cpp)
target_include_directories(skai-annotator PUBLIC include PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(skai-annotator PUBLIC skai-yolo-postprocess PRIVATE ${OpenCV_LIBS})

set(SKAI_ENABLE_TENSORRT "AUTO" CACHE STRING "Build TensorRT loader: AUTO, ON, or OFF")
set_property(CACHE SKAI_ENABLE_TENSORRT PROPERTY STRINGS AUTO ON OFF)
if(NOT SKAI_ENABLE_TENSORRT MATCHES "^(AUTO|ON|OFF)$")
    message(FATAL_ERROR "SKAI_ENABLE_TENSORRT must be AUTO, ON, or OFF")
endif()
if(NOT SKAI_ENABLE_TENSORRT STREQUAL "OFF")
    find_package(CUDAToolkit QUIET)
    find_path(TENSORRT_INCLUDE_DIR NvInfer.h)
    find_library(TENSORRT_LIBRARY nvinfer)
    find_library(TENSORRT_PLUGIN_LIBRARY nvinfer_plugin)
    if(TENSORRT_INCLUDE_DIR AND EXISTS "${TENSORRT_INCLUDE_DIR}/NvInferVersion.h")
        file(STRINGS "${TENSORRT_INCLUDE_DIR}/NvInferVersion.h" TENSORRT_MAJOR_LINE
            REGEX "^#define NV_TENSORRT_MAJOR [0-9]+")
        string(REGEX MATCH "[0-9]+" TENSORRT_MAJOR "${TENSORRT_MAJOR_LINE}")
    endif()
    if(CUDAToolkit_FOUND AND TENSORRT_INCLUDE_DIR AND TENSORRT_LIBRARY AND
            TENSORRT_PLUGIN_LIBRARY AND TENSORRT_MAJOR GREATER_EQUAL 10)
        add_library(skai-tensorrt src/inference/tensorrt_engine.cpp
            src/inference/tensorrt_logger.cpp)
        target_include_directories(skai-tensorrt PUBLIC include ${TENSORRT_INCLUDE_DIR})
        target_include_directories(skai-tensorrt PRIVATE src)
        target_link_libraries(skai-tensorrt PUBLIC skai-tensor-layout skai-logging
            CUDA::cudart ${TENSORRT_LIBRARY} ${TENSORRT_PLUGIN_LIBRARY})
    elseif(SKAI_ENABLE_TENSORRT STREQUAL "ON")
        message(FATAL_ERROR "TensorRT loader requires CUDA Toolkit, TensorRT 10+ headers, libnvinfer and libnvinfer_plugin")
    else()
        message(STATUS "TensorRT/CUDA development files unavailable; skipping skai-tensorrt")
    endif()
endif()

if(TARGET skai-tensorrt)
    include(CheckLanguage)
    check_language(CUDA)
    if(CMAKE_CUDA_COMPILER)
        enable_language(CUDA)
        add_library(skai-preprocess-cuda src/inference/preprocess_cuda.cu)
        set_target_properties(skai-preprocess-cuda PROPERTIES CUDA_STANDARD 17
            CUDA_STANDARD_REQUIRED ON CUDA_ARCHITECTURES 87)
        target_include_directories(skai-preprocess-cuda PUBLIC include)
        target_link_libraries(skai-preprocess-cuda PUBLIC skai-tensorrt
            skai-preprocess-reference CUDA::cudart)
        add_library(skai-yolo-detector src/inference/yolo_detector.cpp)
        target_include_directories(skai-yolo-detector PUBLIC include)
        target_link_libraries(skai-yolo-detector PUBLIC skai-preprocess-cuda
            skai-yolo-postprocess)
        add_library(skai-inference-module src/inference/yolo_inference_module.cpp)
        target_include_directories(skai-inference-module PUBLIC include)
        target_link_libraries(skai-inference-module PUBLIC skai-yolo-detector
            skai-annotator skai-application skai-core skai-events)
    else()
        message(STATUS "CUDA compiler unavailable; skipping skai-preprocess-cuda")
    endif()
endif()

add_library(skai-cli src/cli.cpp)
target_include_directories(skai-cli PUBLIC include)
target_compile_definitions(skai-cli PUBLIC SKAI_EDGE_VERSION="${PROJECT_VERSION}")

find_package(yaml-cpp REQUIRED)
add_library(skai-logging src/logging.cpp)
target_include_directories(skai-logging PUBLIC include)

add_library(skai-config src/config.cpp)
target_include_directories(skai-config PUBLIC include)
target_link_libraries(skai-config PUBLIC yaml-cpp skai-logging)

add_library(skai-application src/application.cpp)
target_include_directories(skai-application PUBLIC include)
target_link_libraries(skai-application PUBLIC skai-config skai-logging Threads::Threads)

add_library(skai-web src/web/http_server.cpp src/web/router.cpp
    src/web/websocket_session.cpp)
target_include_directories(skai-web PUBLIC include)
target_link_libraries(skai-web PUBLIC skai-application skai-events Boost::system)

add_library(skai-gstreamer src/video/gstreamer_runtime.cpp)
target_include_directories(skai-gstreamer PUBLIC include)
target_link_libraries(skai-gstreamer PUBLIC skai-logging PkgConfig::GSTREAMER)

add_library(skai-rtsp-source src/video/rtsp_source.cpp src/video/rtsp_video_module.cpp)
target_include_directories(skai-rtsp-source PUBLIC include)
target_link_libraries(skai-rtsp-source PUBLIC skai-gstreamer skai-config skai-core
    skai-application PkgConfig::GST_VIDEO_APP)

add_executable(skai-edge src/main.cpp)
target_link_libraries(skai-edge PRIVATE skai-cli skai-application skai-gstreamer
    skai-rtsp-source skai-web)
if(TARGET skai-inference-module)
    target_link_libraries(skai-edge PRIVATE skai-inference-module)
    target_compile_definitions(skai-edge PRIVATE SKAI_HAS_YOLO_PIPELINE=1)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

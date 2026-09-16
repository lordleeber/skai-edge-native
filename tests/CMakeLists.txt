find_package(GTest REQUIRED)
include(GoogleTest)
pkg_check_modules(GST_RTSP_TEST REQUIRED IMPORTED_TARGET
    gstreamer-rtsp-server-1.0 gstreamer-app-1.0)

add_executable(skai-unit-tests unit/command_line_test.cpp unit/config_test.cpp
    unit/logging_test.cpp unit/application_test.cpp unit/bounded_queue_test.cpp
    unit/rtsp_recovery_test.cpp unit/tensor_layout_test.cpp
    unit/preprocess_reference_test.cpp unit/yolo_postprocess_test.cpp
    unit/annotator_test.cpp)
target_link_libraries(skai-unit-tests PRIVATE skai-cli skai-config skai-logging
    skai-application skai-core skai-tensor-layout skai-preprocess-reference
    skai-yolo-postprocess skai-annotator
    GTest::gtest_main)
target_compile_definitions(skai-unit-tests PRIVATE
    SKAI_INVALID_CONFIG="${PROJECT_SOURCE_DIR}/tests/fixtures/config.invalid.yaml")
gtest_discover_tests(skai-unit-tests PROPERTIES TIMEOUT 15)

add_executable(skai-integration-tests integration/process_test.cpp)
target_link_libraries(skai-integration-tests PRIVATE skai-rtsp-test-fixture GTest::gtest_main)
target_compile_definitions(skai-integration-tests PRIVATE
    SKAI_EDGE_EXECUTABLE="$<TARGET_FILE:skai-edge>"
    SKAI_INVALID_CONFIG="${PROJECT_SOURCE_DIR}/tests/fixtures/config.invalid.yaml")
add_dependencies(skai-integration-tests skai-edge)
gtest_discover_tests(skai-integration-tests)

add_executable(skai-gstreamer-tests unit/gstreamer_runtime_test.cpp)
target_link_libraries(skai-gstreamer-tests PRIVATE skai-gstreamer GTest::gtest_main)
gtest_discover_tests(skai-gstreamer-tests PROPERTIES TIMEOUT 15)

if(TARGET skai-tensorrt)
    add_executable(skai-tensorrt-tests integration/tensorrt_engine_test.cpp)
    target_include_directories(skai-tensorrt-tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(skai-tensorrt-tests PRIVATE skai-tensorrt GTest::gtest_main)
    gtest_discover_tests(skai-tensorrt-tests PROPERTIES LABELS jetson TIMEOUT 120)
endif()

if(TARGET skai-preprocess-cuda)
    add_executable(skai-preprocess-cuda-tests integration/preprocess_cuda_test.cpp)
    target_link_libraries(skai-preprocess-cuda-tests PRIVATE skai-preprocess-cuda
        GTest::gtest_main)
    target_compile_definitions(skai-preprocess-cuda-tests PRIVATE
        SKAI_YOLO_ENGINE="/var/lib/skai-edge/models/yolo11s_fp16.engine"
        SKAI_YOLO_ONNX="/var/lib/skai-edge/models/yolo11s.onnx")
    gtest_discover_tests(skai-preprocess-cuda-tests PROPERTIES LABELS jetson TIMEOUT 120)
endif()

if(TARGET skai-yolo-detector)
    add_executable(skai-yolo-detector-tests integration/yolo_detector_test.cpp
        integration/annotation_detector_test.cpp)
    target_link_libraries(skai-yolo-detector-tests PRIVATE skai-yolo-detector
        skai-annotator
        GTest::gtest_main)
    target_compile_definitions(skai-yolo-detector-tests PRIVATE
        SKAI_YOLO_ENGINE="/var/lib/skai-edge/models/yolo11s_fp16.engine")
    gtest_discover_tests(skai-yolo-detector-tests PROPERTIES LABELS jetson TIMEOUT 120)
endif()

add_library(skai-rtsp-test-fixture STATIC
    fixtures/rtsp_test_server/rtsp_test_server.cpp)
target_include_directories(skai-rtsp-test-fixture PUBLIC fixtures/rtsp_test_server)
target_link_libraries(skai-rtsp-test-fixture PUBLIC skai-gstreamer
    PkgConfig::GST_RTSP_TEST Threads::Threads)

add_executable(skai-rtsp-fixture-tests integration/rtsp_fixture_test.cpp)
target_link_libraries(skai-rtsp-fixture-tests PRIVATE skai-rtsp-test-fixture
    GTest::gtest_main)
gtest_discover_tests(skai-rtsp-fixture-tests PROPERTIES LABELS rtsp TIMEOUT 30)

add_executable(skai-rtsp-source-tests integration/rtsp_source_test.cpp)
target_link_libraries(skai-rtsp-source-tests PRIVATE skai-rtsp-source
    skai-rtsp-test-fixture GTest::gtest_main)
gtest_discover_tests(skai-rtsp-source-tests PROPERTIES LABELS rtsp TIMEOUT 30)

add_executable(skai-rtsp-jetson-tests integration/rtsp_jetson_test.cpp)
target_link_libraries(skai-rtsp-jetson-tests PRIVATE skai-rtsp-source
    skai-rtsp-test-fixture GTest::gtest_main)
gtest_discover_tests(skai-rtsp-jetson-tests PROPERTIES LABELS "rtsp;jetson" TIMEOUT 30)

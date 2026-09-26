find_package(GTest REQUIRED)
include(GoogleTest)
find_program(SKAI_NODE_EXECUTABLE NAMES node nodejs REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND ${Python3_EXECUTABLE} -c "import yaml" RESULT_VARIABLE SKAI_PYYAML_STATUS)
if(NOT SKAI_PYYAML_STATUS EQUAL 0)
    message(FATAL_ERROR "Smoke script tests require PyYAML; install python3-yaml")
endif()
pkg_check_modules(GST_RTSP_TEST REQUIRED IMPORTED_TARGET
    gstreamer-rtsp-server-1.0 gstreamer-app-1.0)

add_executable(skai-unit-tests unit/command_line_test.cpp unit/config_test.cpp
    unit/logging_test.cpp unit/application_test.cpp unit/bounded_queue_test.cpp
    unit/rtsp_recovery_test.cpp unit/metrics_test.cpp unit/profiling_test.cpp unit/tensor_layout_test.cpp
    unit/preprocess_reference_test.cpp unit/yolo_postprocess_test.cpp
    unit/annotator_test.cpp unit/http_router_test.cpp unit/health_test.cpp unit/event_channel_test.cpp
    unit/gps_source_test.cpp unit/storage_test.cpp unit/alert_manager_test.cpp)
target_link_libraries(skai-unit-tests PRIVATE skai-cli skai-config skai-logging
    skai-application skai-core skai-tensor-layout skai-preprocess-reference
    skai-yolo-postprocess skai-annotator skai-web
    skai-events skai-gps skai-storage skai-alerts
    GTest::gtest_main)
target_compile_definitions(skai-unit-tests PRIVATE
    SKAI_INVALID_CONFIG="${PROJECT_SOURCE_DIR}/tests/fixtures/config.invalid.yaml"
    SKAI_EXAMPLE_CONFIG="${PROJECT_SOURCE_DIR}/config/config.example.yaml")
gtest_discover_tests(skai-unit-tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 15)

add_executable(skai-integration-tests integration/process_test.cpp)
target_link_libraries(skai-integration-tests PRIVATE skai-rtsp-test-fixture GTest::gtest_main)
target_compile_definitions(skai-integration-tests PRIVATE
    SKAI_EDGE_EXECUTABLE="$<TARGET_FILE:skai-edge>"
    SKAI_INVALID_CONFIG="${PROJECT_SOURCE_DIR}/tests/fixtures/config.invalid.yaml")
add_dependencies(skai-integration-tests skai-edge)
gtest_discover_tests(skai-integration-tests DISCOVERY_TIMEOUT 60)

add_executable(skai-http-server-tests integration/http_server_test.cpp)
target_link_libraries(skai-http-server-tests PRIVATE skai-web GTest::gtest_main)
gtest_discover_tests(skai-http-server-tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 15)

add_test(NAME Frontend.LiveStatusStateMachine
    COMMAND ${SKAI_NODE_EXECUTABLE} --test
            ${CMAKE_CURRENT_SOURCE_DIR}/frontend/live_status_ui_test.js)
set_tests_properties(Frontend.LiveStatusStateMachine PROPERTIES TIMEOUT 15)
add_test(NAME Frontend.DiagnosticsPage
    COMMAND ${SKAI_NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/frontend/diagnostics_ui_test.js)
set_tests_properties(Frontend.DiagnosticsPage PROPERTIES TIMEOUT 15)

add_executable(skai-gstreamer-tests unit/gstreamer_runtime_test.cpp)
target_link_libraries(skai-gstreamer-tests PRIVATE skai-gstreamer GTest::gtest_main)
gtest_discover_tests(skai-gstreamer-tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 15)

add_executable(skai-h264-encoder-tests unit/h264_encoder_test.cpp)
target_link_libraries(skai-h264-encoder-tests PRIVATE skai-h264-encoder GTest::gtest_main)
gtest_discover_tests(skai-h264-encoder-tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 15)

add_executable(skai-recording-tests unit/recording_test.cpp)
target_link_libraries(skai-recording-tests PRIVATE skai-recording skai-h264-encoder
    GTest::gtest_main)
gtest_discover_tests(skai-recording-tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 20)

add_executable(skai-detection-sei-tests unit/detection_sei_test.cpp)
target_link_libraries(skai-detection-sei-tests PRIVATE skai-detection-sei skai-gstreamer
    PkgConfig::GST_VIDEO_APP GTest::gtest_main)
target_include_directories(skai-detection-sei-tests PRIVATE support)
gtest_discover_tests(skai-detection-sei-tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 30)

add_executable(skai-webrtc-tests integration/webrtc_dependency_test.cpp
    unit/whip_publisher_test.cpp)
target_include_directories(skai-webrtc-tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(skai-webrtc-tests PRIVATE skai-webrtc-runtime
    LibDataChannel::LibDataChannel GTest::gtest_main)
gtest_discover_tests(skai-webrtc-tests DISCOVERY_TIMEOUT 60 PROPERTIES LABELS webrtc TIMEOUT 15)

add_executable(skai-whip-sei-tests integration/whip_sei_test.cpp)
target_include_directories(skai-whip-sei-tests PRIVATE support)
target_link_libraries(skai-whip-sei-tests PRIVATE skai-webrtc-runtime skai-gstreamer
    skai-health PkgConfig::GST_VIDEO_APP LibDataChannel::LibDataChannel Boost::system GTest::gtest_main)
gtest_discover_tests(skai-whip-sei-tests DISCOVERY_TIMEOUT 60 PROPERTIES LABELS webrtc TIMEOUT 60)

add_executable(skai-whep-tests integration/whep_session_test.cpp)
target_link_libraries(skai-whep-tests PRIVATE skai-web
    LibDataChannel::LibDataChannel GTest::gtest_main)
gtest_discover_tests(skai-whep-tests DISCOVERY_TIMEOUT 60 PROPERTIES LABELS webrtc TIMEOUT 20)

if(TARGET skai-tensorrt)
    add_executable(skai-tensorrt-tests integration/tensorrt_engine_test.cpp)
    target_include_directories(skai-tensorrt-tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(skai-tensorrt-tests PRIVATE skai-tensorrt GTest::gtest_main)
    gtest_discover_tests(skai-tensorrt-tests DISCOVERY_TIMEOUT 60 PROPERTIES LABELS jetson TIMEOUT 120)
endif()

if(TARGET skai-preprocess-cuda)
    add_executable(skai-preprocess-cuda-tests integration/preprocess_cuda_test.cpp)
    target_link_libraries(skai-preprocess-cuda-tests PRIVATE skai-preprocess-cuda
        GTest::gtest_main)
    target_compile_definitions(skai-preprocess-cuda-tests PRIVATE
        SKAI_YOLO_ENGINE="/var/lib/skai-edge/models/yolo11s_fp16.engine"
        SKAI_YOLO_ONNX="/var/lib/skai-edge/models/yolo11s.onnx")
    gtest_discover_tests(skai-preprocess-cuda-tests DISCOVERY_TIMEOUT 60 PROPERTIES LABELS jetson TIMEOUT 120)
endif()

if(TARGET skai-yolo-detector)
    add_executable(skai-yolo-detector-tests integration/yolo_detector_test.cpp
        integration/annotation_detector_test.cpp)
    target_link_libraries(skai-yolo-detector-tests PRIVATE skai-yolo-detector
        skai-annotator skai-inference-module skai-rtsp-source
        skai-rtsp-test-fixture
        GTest::gtest_main)
    target_compile_definitions(skai-yolo-detector-tests PRIVATE
        SKAI_YOLO_ENGINE="/var/lib/skai-edge/models/yolo11s_fp16.engine")
    gtest_discover_tests(skai-yolo-detector-tests DISCOVERY_TIMEOUT 60 PROPERTIES LABELS jetson TIMEOUT 120)
endif()

add_library(skai-rtsp-test-fixture STATIC
    fixtures/rtsp_test_server/rtsp_test_server.cpp)
target_include_directories(skai-rtsp-test-fixture PUBLIC fixtures/rtsp_test_server)
target_link_libraries(skai-rtsp-test-fixture PUBLIC skai-gstreamer
    PkgConfig::GST_RTSP_TEST Threads::Threads)

add_executable(skai-rtsp-fixture-tests integration/rtsp_fixture_test.cpp)
target_link_libraries(skai-rtsp-fixture-tests PRIVATE skai-rtsp-test-fixture
    GTest::gtest_main)
gtest_discover_tests(skai-rtsp-fixture-tests DISCOVERY_TIMEOUT 60 PROPERTIES LABELS rtsp TIMEOUT 30)

add_executable(skai-rtsp-source-tests integration/rtsp_source_test.cpp)
target_link_libraries(skai-rtsp-source-tests PRIVATE skai-rtsp-source
    skai-rtsp-test-fixture GTest::gtest_main)
gtest_discover_tests(skai-rtsp-source-tests DISCOVERY_TIMEOUT 60 PROPERTIES LABELS rtsp TIMEOUT 30)

add_executable(skai-rtsp-jetson-tests integration/rtsp_jetson_test.cpp)
target_link_libraries(skai-rtsp-jetson-tests PRIVATE skai-rtsp-source
    skai-rtsp-test-fixture GTest::gtest_main)
# A single compound label survives GoogleTest's discovery script and matches both
# `ctest -L rtsp` and `ctest -LE jetson` on CMake 3.22.
gtest_discover_tests(skai-rtsp-jetson-tests DISCOVERY_TIMEOUT 60 PROPERTIES LABELS rtsp-jetson TIMEOUT 30)

# Step 34 composes production modules around a deterministic RTSP fixture.
add_executable(skai-pipeline-tests integration/pipeline_test.cpp)
target_include_directories(skai-pipeline-tests PRIVATE support ${OpenCV_INCLUDE_DIRS})
target_link_libraries(skai-pipeline-tests PRIVATE skai-runtime skai-web skai-alerts skai-recording
    skai-rtsp-source skai-rtsp-test-fixture ${OpenCV_LIBS}
    LibDataChannel::LibDataChannel GTest::gtest_main)
gtest_discover_tests(skai-pipeline-tests DISCOVERY_TIMEOUT 60
    PROPERTIES LABELS "rtsp-webrtc" TIMEOUT 60)

if(TARGET skai-yolo-detector)
    add_executable(skai-pipeline-jetson-tests integration/pipeline_test.cpp)
    target_compile_definitions(skai-pipeline-jetson-tests PRIVATE SKAI_PIPELINE_TENSORRT=1)
    target_include_directories(skai-pipeline-jetson-tests PRIVATE support ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(skai-pipeline-jetson-tests PRIVATE skai-runtime skai-web skai-alerts
        skai-recording skai-rtsp-source skai-rtsp-test-fixture skai-inference-module
        ${OpenCV_LIBS} LibDataChannel::LibDataChannel GTest::gtest_main)
    gtest_discover_tests(skai-pipeline-jetson-tests DISCOVERY_TIMEOUT 60
        PROPERTIES LABELS "rtsp-jetson" TIMEOUT 120)
endif()

add_executable(skai-runtime-factory-tests integration/runtime_factory_test.cpp)
target_link_libraries(skai-runtime-factory-tests PRIVATE skai-runtime GTest::gtest_main)
gtest_discover_tests(skai-runtime-factory-tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 20)

add_executable(skai-smoke-script-tests integration/smoke_script_test.cpp)
target_link_libraries(skai-smoke-script-tests PRIVATE GTest::gtest_main)
target_compile_definitions(skai-smoke-script-tests PRIVATE
    SKAI_PYTHON="${Python3_EXECUTABLE}"
    SKAI_SMOKE_SCRIPT="${PROJECT_SOURCE_DIR}/scripts/smoke_test_jetson.sh"
    SKAI_SMOKE_FIXTURE="${PROJECT_SOURCE_DIR}/tests/fixtures/smoke_service.py"
    SKAI_SMOKE_HOST_POLICY="${PROJECT_SOURCE_DIR}/tests/fixtures/smoke_host_policy.py")
gtest_discover_tests(skai-smoke-script-tests DISCOVERY_TIMEOUT 60 PROPERTIES
    LABELS smoke TIMEOUT 20 ENVIRONMENT "SKAI_SMOKE_PYTHON=${Python3_EXECUTABLE}")

find_program(SKAI_SYSTEMD_ANALYZE NAMES systemd-analyze)
if(NOT SKAI_SYSTEMD_ANALYZE)
    set(SKAI_SYSTEMD_ANALYZE "")
endif()
add_executable(skai-systemd-service-tests unit/systemd_service_test.cpp)
target_link_libraries(skai-systemd-service-tests PRIVATE GTest::gtest_main)
target_compile_definitions(skai-systemd-service-tests PRIVATE
    SKAI_SYSTEMD_UNIT="${PROJECT_SOURCE_DIR}/systemd/skai-edge.service"
    SKAI_SYSTEMD_ANALYZE="${SKAI_SYSTEMD_ANALYZE}"
    SKAI_EDGE_EXECUTABLE="$<TARGET_FILE:skai-edge>")
add_dependencies(skai-systemd-service-tests skai-edge)
gtest_discover_tests(skai-systemd-service-tests DISCOVERY_TIMEOUT 60
    PROPERTIES LABELS systemd TIMEOUT 15)

add_executable(skai-install-tests integration/install_test.cpp)
target_include_directories(skai-install-tests PRIVATE support)
target_link_libraries(skai-install-tests PRIVATE skai-config SQLite::SQLite3 GTest::gtest_main)
target_compile_definitions(skai-install-tests PRIVATE
    SKAI_CMAKE="${CMAKE_COMMAND}" SKAI_BUILD_DIR="${PROJECT_BINARY_DIR}"
    SKAI_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_dependencies(skai-install-tests skai-edge)
gtest_discover_tests(skai-install-tests DISCOVERY_TIMEOUT 60
    PROPERTIES LABELS install RUN_SERIAL TRUE TIMEOUT 60)

add_executable(skai-installed-runtime-tests integration/installed_runtime_test.cpp)
target_include_directories(skai-installed-runtime-tests PRIVATE support)
target_link_libraries(skai-installed-runtime-tests PRIVATE skai-rtsp-test-fixture GTest::gtest_main)
target_compile_definitions(skai-installed-runtime-tests PRIVATE
    SKAI_CMAKE="${CMAKE_COMMAND}" SKAI_BUILD_DIR="${PROJECT_BINARY_DIR}"
    SKAI_PYTHON="${Python3_EXECUTABLE}"
    SKAI_INSTALL_RUNTIME_PROBE="${PROJECT_SOURCE_DIR}/tests/fixtures/installed_runtime.py"
    SKAI_INSTALL_ENGINE="/var/lib/skai-edge/models/yolo11s_fp16.engine")
add_dependencies(skai-installed-runtime-tests skai-edge)
if(TARGET skai-yolo-detector)
    target_compile_definitions(skai-installed-runtime-tests PRIVATE SKAI_INSTALL_GPU=1)
    set(SKAI_INSTALL_RUNTIME_LABEL install-jetson)
else()
    target_compile_definitions(skai-installed-runtime-tests PRIVATE SKAI_INSTALL_GPU=0)
    set(SKAI_INSTALL_RUNTIME_LABEL install)
endif()
gtest_discover_tests(skai-installed-runtime-tests DISCOVERY_TIMEOUT 60
    PROPERTIES LABELS ${SKAI_INSTALL_RUNTIME_LABEL} RUN_SERIAL TRUE TIMEOUT 60)

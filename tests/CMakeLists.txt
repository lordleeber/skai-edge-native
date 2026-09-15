find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(skai-unit-tests unit/command_line_test.cpp unit/config_test.cpp
    unit/logging_test.cpp unit/application_test.cpp unit/bounded_queue_test.cpp)
target_link_libraries(skai-unit-tests PRIVATE skai-cli skai-config skai-logging
    skai-application skai-core GTest::gtest_main)
target_compile_definitions(skai-unit-tests PRIVATE
    SKAI_INVALID_CONFIG="${PROJECT_SOURCE_DIR}/tests/fixtures/config.invalid.yaml")
gtest_discover_tests(skai-unit-tests PROPERTIES TIMEOUT 15)

add_executable(skai-integration-tests integration/process_test.cpp)
target_link_libraries(skai-integration-tests PRIVATE GTest::gtest_main)
target_compile_definitions(skai-integration-tests PRIVATE
    SKAI_EDGE_EXECUTABLE="$<TARGET_FILE:skai-edge>"
    SKAI_EXAMPLE_CONFIG="${PROJECT_SOURCE_DIR}/config/config.example.yaml"
    SKAI_INVALID_CONFIG="${PROJECT_SOURCE_DIR}/tests/fixtures/config.invalid.yaml")
add_dependencies(skai-integration-tests skai-edge)
gtest_discover_tests(skai-integration-tests)

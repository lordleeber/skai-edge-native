find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(skai-unit-tests unit/command_line_test.cpp)
target_link_libraries(skai-unit-tests PRIVATE skai-cli GTest::gtest_main)
gtest_discover_tests(skai-unit-tests)

add_executable(skai-integration-tests integration/process_test.cpp)
target_link_libraries(skai-integration-tests PRIVATE GTest::gtest_main)
target_compile_definitions(skai-integration-tests PRIVATE
    SKAI_EDGE_EXECUTABLE="$<TARGET_FILE:skai-edge>")
add_dependencies(skai-integration-tests skai-edge)
gtest_discover_tests(skai-integration-tests)

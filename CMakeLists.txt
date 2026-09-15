cmake_minimum_required(VERSION 3.22)

project(skai-edge-native VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(skai-cli src/cli.cpp)
target_include_directories(skai-cli PUBLIC include)
target_compile_definitions(skai-cli PUBLIC SKAI_EDGE_VERSION="${PROJECT_VERSION}")

find_package(yaml-cpp REQUIRED)
add_library(skai-logging src/logging.cpp)
target_include_directories(skai-logging PUBLIC include)

add_library(skai-config src/config.cpp)
target_include_directories(skai-config PUBLIC include)
target_link_libraries(skai-config PUBLIC yaml-cpp skai-logging)

add_executable(skai-edge src/main.cpp)
target_link_libraries(skai-edge PRIVATE skai-cli skai-config skai-logging)

enable_testing()
add_subdirectory(tests)

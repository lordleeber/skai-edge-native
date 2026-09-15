cmake_minimum_required(VERSION 3.22)

project(skai-edge-native VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(Threads REQUIRED)

add_library(skai-core INTERFACE)
target_include_directories(skai-core INTERFACE include)
target_link_libraries(skai-core INTERFACE Threads::Threads)

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

add_executable(skai-edge src/main.cpp)
target_link_libraries(skai-edge PRIVATE skai-cli skai-application)

enable_testing()
add_subdirectory(tests)

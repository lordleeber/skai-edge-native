# Repository Guidelines

## Project Structure & Module Organization

This C++17 Jetson service keeps public APIs in `include/skai/` (`core/` queues, `video/` RTSP/GStreamer) and implementations and `main.cpp` in `src/`. `Application` owns the RTSP video module; web, detector, and GPS modules are future work. Unit tests are in `tests/unit/`, executable and source tests in `tests/integration/`, and the test-only RTSP server in `tests/fixtures/rtsp_test_server/`. `config/` contains example YAML; `models/`, `web/`, `systemd/`, `scripts/`, `cmake/`, `docs/`, and `third_party/` are placeholders. Read `ROADMAP.md` before adding those features; inference is not implemented yet.

## Build, Test, and Development Commands

Install CMake 3.22+, a C++17 compiler, GoogleTest (`libgtest-dev`), yaml-cpp (`libyaml-cpp-dev`), and GStreamer development packages (`libgstreamer1.0-dev`, `libgstreamer-plugins-base1.0-dev`, `libgstrtspserver-1.0-dev`). Install H.264 encoder plugins and `gstreamer1.0-libav` for software decode; H.265 test plugins are optional. From the repository root:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/skai-edge --help
./build/skai-edge --version
./build/skai-edge --config config/config.example.yaml
```

The first two commands configure and build the executable and tests; CTest runs the discovered GoogleTest cases. Running `./build/skai-edge` without options tries the built-in loopback RTSP URL. `--config` validates YAML and connects to its RTSP URL before readiness. Keep generated files under the ignored `build/` directory.

## Coding Style & Naming Conventions

Use C++17 without compiler extensions, as configured in `CMakeLists.txt`. Follow the existing four-space indentation, `snake_case` for functions and variables, `PascalCase` for types and enum values, and the `skai` namespace for project APIs. Put public declarations under `include/skai/` and matching implementations under `src/`. No formatter or linter is configured; match nearby code and keep changes focused.

## Testing Guidelines

Use GoogleTest assertions (`TEST`, `EXPECT_*`, `ASSERT_*`). Name test files `*_test.cpp` and cases by behavior, as in `TEST(RtspSource, ReceivesH264OverUdp)`. For each roadmap PR, write tests first and confirm they fail; implement until they pass, then keep them passing while refactoring. Add unit tests for isolated logic and integration tests for executable behavior. RTSP tests carry the `rtsp` CTest label; hardware tests also carry `jetson`. Run `ctest --test-dir build --output-on-failure` before submitting. There is no configured coverage threshold.

## Commit & Pull Request Guidelines

Recent commits use short, imperative subjects such as `Build PR 1 repository skeleton and lifecycle tests` and `Add project roadmap`. Use a similarly descriptive subject. In pull requests, explain the behavior changed, identify the roadmap stage or related issue when applicable, and include build and test results. Include screenshots only for visible UI changes.

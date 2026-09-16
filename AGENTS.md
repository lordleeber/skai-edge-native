# Repository Guidelines

## Project Structure & Module Organization

Public APIs live in `include/skai/` (`core/` queues, `video/` RTSP, `inference/` TensorRT); implementations and `main.cpp` live in `src/`. Unit tests are in `tests/unit/`, integration tests in `tests/integration/`, and the RTSP fixture in `tests/fixtures/rtsp_test_server/`. `config/` has example YAML; `models/` and other module directories await later steps. Read `ROADMAP.md` before adding features.

## Build, Test, and Development Commands

Install CMake 3.22+, a C++17 compiler, GoogleTest (`libgtest-dev`), yaml-cpp (`libyaml-cpp-dev`), OpenCV (`libopencv-dev`), Boost.System (`libboost-system-dev`), and GStreamer development packages (`libgstreamer1.0-dev`, `libgstreamer-plugins-base1.0-dev`, `libgstrtspserver-1.0-dev`). Install H.264 encoder plugins and `gstreamer1.0-libav` for software decode; H.265 test plugins are optional. From the root:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/skai-edge --help
./build/skai-edge --version
./build/skai-edge --config config/config.example.yaml
./build/skai-edge --rtsp-test --config config/config.example.yaml
```

The first two commands configure and build; CTest runs GoogleTest cases. Without options, `skai-edge` tries the loopback RTSP URL. `--config` validates YAML, and `--rtsp-test` prints source metrics. Keep generated files in ignored `build/`.

## Coding Style & Naming Conventions

Use C++17 without compiler extensions, as configured in `CMakeLists.txt`. Follow the existing four-space indentation, `snake_case` for functions and variables, `PascalCase` for types and enum values, and the `skai` namespace for project APIs. Put public declarations under `include/skai/` and matching implementations under `src/`. No formatter or linter is configured; match nearby code and keep changes focused.

## Testing Guidelines

Use GoogleTest assertions (`TEST`, `EXPECT_*`, `ASSERT_*`). Name test files `*_test.cpp` and cases by behavior, as in `TEST(RtspSource, ReceivesH264OverUdp)`. For each roadmap step, write failing tests first, then implement and refactor while they pass. If implementation and test code exceeds 800 changed lines, split it into `step-N-a`, `step-N-b`, and so on; keep each at or below 800 lines with its own tests. Add unit tests for isolated logic and integration tests for executable behavior. RTSP tests carry the `rtsp` CTest label; hardware tests carry `jetson`. TensorRT is optional in CMake; use `-DSKAI_ENABLE_TENSORRT=ON` to require its CUDA and TensorRT development files. Run `ctest --test-dir build --output-on-failure` before submitting. There is no configured coverage threshold.

## Commit & Pull Request Guidelines

Recent commits use short, imperative subjects such as `Build PR 1 repository skeleton and lifecycle tests` and `Add project roadmap`. Use a similarly descriptive subject. In pull requests, explain the behavior changed, identify the roadmap stage or related issue when applicable, and include build and test results. Include screenshots only for visible UI changes.

For each completed roadmap step, commit the tested changes on its feature branch, push the branch, and open a pull request immediately. The user has authorized this workflow for future steps; no separate request is needed to open the PR. Merging remains a separate decision.

When the user asks "看看 code reviewer 說的是否合理", review each finding and implement the ones supported by the code and expected behavior. Add regression tests, run the relevant checks, then commit and push the fixes to the current feature branch so its open PR updates. The final response should briefly say which findings were reasonable and fixed, and which were not reasonable and left unchanged. This review-and-fix workflow is already authorized; merging remains a separate decision.

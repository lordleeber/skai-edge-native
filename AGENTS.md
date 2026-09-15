# Repository Guidelines

## Project Structure & Module Organization

This repository currently builds a small C++17 Jetson edge-service skeleton. Public interfaces live in `include/skai/`; implementations and the executable entry point live in `src/`. Unit tests are in `tests/unit/`, and executable lifecycle tests are in `tests/integration/`. `config/`, `models/`, `web/`, `systemd/`, `scripts/`, `cmake/`, `docs/`, and `third_party/` are placeholders for later stages. Read `ROADMAP.md` before adding those features; the current service has no RTSP ingest or inference.

## Build, Test, and Development Commands

Install CMake 3.22 or newer, a C++17 compiler, and GoogleTest (for example, Ubuntu's `libgtest-dev`). From the repository root:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/skai-edge --help
./build/skai-edge --version
```

The first two commands configure and build the executable and tests; CTest runs the discovered GoogleTest cases. Running `./build/skai-edge` without options starts the service until SIGINT or SIGTERM. Keep generated files under the ignored `build/` directory.

## Coding Style & Naming Conventions

Use C++17 without compiler extensions, as configured in `CMakeLists.txt`. Follow the existing four-space indentation, `snake_case` for functions and variables, `PascalCase` for types and enum values, and the `skai` namespace for project APIs. Put public declarations under `include/skai/` and matching implementations under `src/`. No formatter or linter is configured; match nearby code and keep changes focused.

## Testing Guidelines

Use GoogleTest assertions (`TEST`, `EXPECT_*`, `ASSERT_*`). Name test files `*_test.cpp` and cases by behavior, as in `TEST(CommandLine, HelpPrintsUsage)`. Add unit tests for parsing or other isolated logic and integration tests when executable output, exit status, or signals matter. Run `ctest --test-dir build --output-on-failure` before submitting. There is no configured coverage threshold.

## Commit & Pull Request Guidelines

Recent commits use short, imperative subjects such as `Build PR 1 repository skeleton and lifecycle tests` and `Add project roadmap`. Use a similarly descriptive subject. In pull requests, explain the behavior changed, identify the roadmap stage or related issue when applicable, and include build and test results. Include screenshots only for visible UI changes.

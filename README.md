# skai-edge-native

A clean-room C++17 edge AI service for NVIDIA Jetson. The only planned video
input is one configured RTSP URL. See [ROADMAP.md](ROADMAP.md) for the staged
implementation and architectural rules.

PR 1 provides the native executable, command-line options, GoogleTest/CTest,
and graceful SIGINT/SIGTERM shutdown. It does not yet ingest video.

## Build and test

Install CMake, a C++17 compiler, and GoogleTest (`libgtest-dev` on Ubuntu), then:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/skai-edge --version
```

Run `./build/skai-edge --help` for usage. With no arguments, the service waits
for SIGINT or SIGTERM and exits cleanly. Future PRs will add configuration,
RTSP ingest, inference, and web APIs.

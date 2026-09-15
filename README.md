# skai-edge-native

A clean-room C++17 edge AI service for NVIDIA Jetson. The only planned video
input is one configured RTSP URL. See [ROADMAP.md](ROADMAP.md) for the staged
implementation and architectural rules.

PR 1 provides the native executable, command-line options, GoogleTest/CTest,
and graceful SIGINT/SIGTERM shutdown. PR 2 adds validated YAML configuration
and structured logging. PR 3 adds an Application lifecycle that loads config,
initializes modules in web/detector/video/GPS order, starts their workers, and
stops and joins them in reverse order. PR 4 adds a bounded producer/consumer
queue for future real-time paths. The modules are lifecycle hooks for later
PRs; this service does not yet ingest video.

## Build and test

Install CMake 3.22+, a C++17 compiler, GoogleTest (`libgtest-dev` on Ubuntu),
and yaml-cpp (`libyaml-cpp-dev`), then:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/skai-edge --version
./build/skai-edge --config config/config.example.yaml
```

Run `./build/skai-edge --help` for usage. With no arguments, the service uses
built-in defaults; `--config PATH` loads and validates a YAML file before it
reports readiness. Copy `config/config.example.yaml` and set one real RTSP URL
for deployment. Invalid files produce an error with the field name and exit
before readiness. Logs use UTC timestamps and `level`, `module`, and `message`
fields; set `logging.level` to `trace`, `debug`, `info`, `warning`, or `error`.
The service waits for SIGINT or SIGTERM and exits cleanly. Future PRs will add
RTSP ingest, inference, and web APIs. The example URL uses the provided test
endpoint; PR 2 validates its syntax without contacting the server.

## Bounded queue

`skai::BoundedQueue<T>` is a header-only primitive in
`include/skai/core/bounded_queue.hpp`. Give it a positive capacity (2 or 3 for
fresh video frames). `push()` drops the oldest queued value when full;
`pop()` blocks and `pop_for(timeout)` can time out. Call `shutdown()` to reject
new pushes and wake waiting consumers. Existing values can still be drained;
an empty optional means the queue is drained after shutdown or a timed wait
expired. `stats()` reports pushed, popped, dropped, and high-water counts.

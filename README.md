# skai-edge-native

A clean-room C++17 edge AI service for NVIDIA Jetson. The only planned video
input is one configured RTSP URL. See [ROADMAP.md](ROADMAP.md) for the staged
implementation and architectural rules.

PR 1 provides the native executable, command-line options, GoogleTest/CTest,
and graceful SIGINT/SIGTERM shutdown. PR 2 adds validated YAML configuration
and structured logging. PR 3 adds an Application lifecycle that loads config,
initializes modules in web/detector/video/GPS order, starts their workers, and
stops and joins them in reverse order. PR 4 adds a bounded producer/consumer
queue for future real-time paths. PR 5 adds a GStreamer runtime wrapper and a
test-only loopback RTSP server. PR 6 adds the single RTSP input module: it
decodes H.264/H.265 into packed BGR frames in a bounded inference queue. Web,
inference, and GPS modules are still future work. PR 7 adds automatic recovery,
frame-freshness stall detection, and JSON RTSP diagnostics.

## Build and test

Install CMake 3.22+, a C++17 compiler, yaml-cpp (`libyaml-cpp-dev`),
GStreamer development packages (`libgstreamer1.0-dev`,
`libgstreamer-plugins-base1.0-dev`), and GStreamer plugins from the base, good, ugly
(H.264), libav (software H.264/H.265 decode), and bad (optional H.265) sets,
then install GoogleTest (`libgtest-dev`) and the test-only RTSP server
development package (`libgstrtspserver-1.0-dev`) to run tests:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/skai-edge --version
./build/skai-edge --config config/config.example.yaml
./build/skai-edge --rtsp-test --config config/config.example.yaml
```

For an executable-only build without GoogleTest or the RTSP-server fixture:

```sh
cmake -S . -B build/production -DBUILD_TESTING=OFF
cmake --build build/production --target skai-edge
```

Run `./build/skai-edge --help` for usage. With no arguments, the service tries
the built-in RTSP URL `rtsp://127.0.0.1/stream`; provide `--config PATH` for a
real stream. Copy `config/config.example.yaml` and set `video.rtsp_url` for
deployment. Startup validates the URL, then retries in the background if the
endpoint is unavailable. Set
`video.transport` to `tcp` or `udp` and `video.latency_ms` for the RTSP
jitterbuffer. Credentials may be embedded in the URL or supplied as
`video.username` and `video.password` in YAML. Keep credential-bearing files
out of Git. Logs use UTC timestamps and `level`, `module`, and `message`
fields. The service waits for SIGINT or SIGTERM and exits cleanly. Set
`video.reconnect_delay_ms`, `video.max_reconnect_delay_ms`, and
`video.stall_timeout_ms` to tune recovery; `video.first_frame_timeout_ms`
allows longer initial waits for a keyframe. `--rtsp-test` skips inference and
prints one JSON metrics line per second to stdout with health, frame age,
dropped frames, stale frames discarded on reconnect, reconnect count, and RTP
jitter statistics when available;
diagnostic logs go to stderr.

## GStreamer runtime and RTSP fixture

`main` initializes GStreamer once before the Application starts. The wrapper
in `include/skai/video/gstreamer_runtime.hpp` owns pipeline elements and buses,
reports PLAYING/NULL transitions, and parses EOS or ERROR bus messages. The
fixture under `tests/fixtures/rtsp_test_server/` publishes a generated H.264
stream (and H.265 when its plugins are installed) at
`rtsp://127.0.0.1:<ephemeral-port>/test`. Tests can stop, restart, or stall it;
the fixture is linked only to test executables. Run its tests with
`ctest --test-dir build -L rtsp --output-on-failure`. The fixture can also
require Basic authentication for source tests. On Jetson, the source selects
`nvv4l2decoder` with NVIDIA conversion when available; software decoding is
used elsewhere. The `RtspSource` API exposes frame sequence, capture timestamp,
codec, decoder, resolution, FPS, frame count, and health diagnostics. FPS is
reported as unknown when the upstream stream does not advertise a frame rate.

## Bounded queue

`skai::BoundedQueue<T>` is a header-only primitive in
`include/skai/core/bounded_queue.hpp`. Give it a positive capacity (2 or 3 for
fresh video frames). `push()` drops the oldest queued value when full;
`pop()` blocks and `pop_for(timeout)` can time out. Call `shutdown()` to reject
new pushes and wake waiting consumers. Existing values can still be drained;
an empty optional means the queue is drained after shutdown or a timed wait
expired. After all producers and consumers have joined, `reset()` clears stale
values and reopens the queue for another application lifecycle. `stats()`
reports pushed, popped, dropped, and high-water counts for the current cycle.

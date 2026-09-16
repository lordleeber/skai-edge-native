# skai-edge-native

A clean-room C++17 edge AI service for NVIDIA Jetson. The only planned video
input is one configured RTSP URL. See [ROADMAP.md](ROADMAP.md) for the staged
implementation and architectural rules.

Step 1 provides the native executable, command-line options, GoogleTest/CTest,
and graceful SIGINT/SIGTERM shutdown. Step 2 adds validated YAML configuration
and structured logging. Step 3 adds an Application lifecycle that loads config,
initializes modules in web/detector/video/GPS order, starts their workers, and
stops and joins them in reverse order. Step 4 adds a bounded producer/consumer
queue for future real-time paths. Step 5 adds a GStreamer runtime wrapper and a
test-only loopback RTSP server. Step 6 adds the single RTSP input module: it
decodes H.264/H.265 into packed BGR frames in a bounded inference queue. Web
and GPS modules are still future work. Step 7 adds automatic recovery,
frame-freshness stall detection, and JSON RTSP diagnostics. Step 8 adds an
independent TensorRT engine loader.
Step 9 adds CPU reference letterbox/NCHW conversion and a fused CUDA path
validated against the local YOLO11s TensorRT engine.
Step 10 adds YOLO11 inference and postprocessing. Step 11 connects the RTSP
frame queue to a TensorRT worker and adds CPU/OpenCV annotation for detections
and optional timing. Step 12 adds the asynchronous Boost.Beast HTTP foundation.
Encoding and external media transport remain later steps.

## Build and test

Install CMake 3.22+, a C++17 compiler, yaml-cpp (`libyaml-cpp-dev`),
Boost.System development files (`libboost-system-dev`),
GStreamer development packages (`libgstreamer1.0-dev`,
`libgstreamer-plugins-base1.0-dev`), and GStreamer plugins from the base, good, ugly
(H.264), libav (software H.264/H.265 decode), and bad (optional H.265) sets,
OpenCV development files (`libopencv-dev`), then install GoogleTest
(`libgtest-dev`) and the test-only RTSP server
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

## HTTP status server

The service binds `web.bind` and `web.port` with an asynchronous Boost.Asio /
Boost.Beast server. `GET /health` returns the Step 12 liveness response and
`GET /api/v1/status` returns JSON containing service uptime plus reserved video
and detector metric fields. Unknown routes return 404 and unsupported methods
return 405. Requests have fixed 16 KiB header and 64 KiB body limits and a
five-second read/write timeout. The server stops through the normal Application
lifecycle. WebSocket, static files, and the wider REST API belong to later steps.

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

## TensorRT engine loader

On a Jetson with CUDA Toolkit and TensorRT 10 development files, CMake builds
the `skai-tensorrt` library automatically. Set `-DSKAI_ENABLE_TENSORRT=ON` to
require those dependencies or `OFF` for a build without them. Run
`ctest --test-dir build -L jetson --output-on-failure` for the hardware tests;
the loader test builds a small engine locally, so no model download is needed.

`skai::TensorRtEngine` loads a trusted serialized engine, checks that it has
named input and output tensors, and reports their names, shapes, types and byte
sizes. It owns one GPU buffer per I/O tensor and a CUDA stream, and exposes the
native engine for a later inference consumer. The caller selects the CUDA
device before loading and keeps that device active through destruction. For
engines using NVIDIA standard plugins, the application must create one
`TensorRtBootstrap` at process startup, call
`initialize_standard_plugins()` once before engine loading, and keep the
bootstrap alive until its engines are destroyed. Engine reloads do not
re-register plugins. Step 8 accepts fixed-shape, linear, device-resident
tensors; dynamic profiles,
vectorized formats, host shape tensors and packed INT4 are rejected with a
specific error until their sizing and address rules are implemented. The
When TensorRT/CUDA support is available, the service loads `detector.engine`
through its inference module during startup.

## Preprocessing reference

`skai::make_preprocess_plan()` computes centered letterbox geometry from a
borrowed packed BGR frame and target dimensions. `preprocess_cpu()` uses
bilinear resize, padding value 114, RGB channel order, `1/255` normalization,
and FP32 NCHW layout. The returned plan retains scale and padding for Step 10
box-coordinate restoration. The planned local `yolo11s_fp16.engine` binding is
FP32 `images` with shape `1×3×640×640`; model files stay outside Git under
`/var/lib/skai-edge/models/`.

When NVCC is available with TensorRT, CMake also builds
`skai-preprocess-cuda` for Orin (CUDA architecture 8.7). A single inference
worker can reuse `skai::CudaPreprocessor` to upload a BGR frame and write the
loaded engine's FP32 `images` buffer. `run()` returns the letterbox plan,
CUDA event time for upload plus conversion, and host wall time; the input
buffer is ready on return. Hardware tests compare the full tensor against the
CPU reference using `/var/lib/skai-edge/models/yolo11s_fp16.engine` and check
that `yolo11s.onnx` is present. Run them with
`ctest --test-dir build -R CudaPreprocess --output-on-failure`.

## YOLO11 inference and postprocessing

`skai::YoloDetector` owns a TensorRT engine, execution context, and reusable
CUDA preprocessor. After `load(engine_path)`, `run(image, frame_sequence, ...)`
returns a `DetectionResult` and timing for preprocessing, inference, and CPU
postprocessing. A process-level `TensorRtBootstrap` must outlive the detector.
The supported engine contract is FP32 `images` `[1,3,H,W]` and raw FP32
`output0` `[1,4+classes,candidates]`; embedded-NMS or dynamic-shape engines
are outside this step.

The GPU-independent `postprocess_yolo()` selects the highest class score per
candidate, filters by confidence, applies per-class NMS, and restores/clips
boxes to the original frame using the integer letterbox dimensions. Configure
its confidence and NMS thresholds through `YoloPostprocessConfig`; callers
can populate them from the parsed YAML `detector.confidence` and `detector.nms`
values. The Jetson test compares a reproducible image with independently
evaluated `yolo11s.onnx` reference values and reruns it for deterministic output.

## Frame annotation

`skai::annotate_frame()` takes a decoded BGR `Frame`, the matching
`DetectionResult`, optional class names, and `AnnotationOptions`. It copies the
frame before OpenCV draws bounding boxes, class/confidence labels, and optional
FPS/inference timing, so the input pixels and inference result remain unchanged.
Class IDs without supplied names render as `class_<id>`. Set
`options.enabled = false` for a pixel-identical copy; the YAML
`detector.annotate` flag defaults to true. `YoloInferenceModule` consumes the
RTSP inference queue, runs TensorRT YOLO, applies that setting, and publishes
the resulting frame to a bounded queue for the next media stage. The standard
YOLO11 COCO class names are used by this service path. Encoding and external
video delivery are not implemented yet.

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

# ROADMAP.md

# skai-edge-native

A clean-room C++ rebuild of the edge AI runtime, designed for NVIDIA Jetson.

**Video input contract: exactly one RTSP URL. No other input source is supported.**

The new implementation intentionally removes ROS 2, Agora, Flask, FastAPI, and frontend frameworks.  
The application runs as a single native C++ service with explicit in-process module boundaries and accepts video only from a configured RTSP URL.

---

## 1. Project Goals

Build a lightweight Jetson edge application whose only video input is an RTSP URL and that provides:

- GStreamer-based RTSP ingest and media pipelines
- TensorRT-based object detection
- annotated video output
- local recording
- GPS position (fixed at Taipei 101 in the first version; no GPS hardware)
- alert/event generation
- REST-style HTTP APIs
- WebSocket event/status updates
- browser-based monitoring UI
- optional browser live video through WebRTC
- systemd deployment on Jetson

The system should be understandable from end to end without ROS 2, Python web frameworks, or a JavaScript framework.

Primary target:

- NVIDIA Jetson Orin Nano
- Ubuntu / JetPack 6
- C++17 or newer
- CMake
- GStreamer
- TensorRT / CUDA
- Boost.Asio + Boost.Beast
- SQLite
- libdatachannel
- skai-ice (custom STUN/ICE implementation)
- vanilla HTML/CSS/JavaScript

---

## 2. Explicit Non-Goals

The initial implementation will NOT use:

- ROS 2
- DDS
- ROS topics/services/actions
- Agora
- Flask
- FastAPI
- Node.js backend
- React / Vue / Angular / Svelte
- Python as a runtime dependency
- a distributed microservice architecture
- GStreamer `webrtcbin` as the browser WebRTC stack
- a second ICE implementation inside this repository
- TURN / srflx / Internet-NAT traversal in the first version
- USB / V4L2 camera input
- MIPI CSI / Argus camera input
- local video file input
- generic pluggable video-source abstraction
- real GPS hardware (serial UBX/NMEA, MAVLink) in the first version

The first version should prefer one native process with clear internal modules.

Do not recreate ROS 2 inside the application by building an unnecessarily complicated generic message bus.

---

## 3. High-Level Architecture

```text
                              +----------------------+
                              |      Browser UI      |
                              | HTML/CSS/JavaScript  |
                              +----------+-----------+
                                         |
                         HTTP / REST / WebSocket / WHEP
                                         |
                                         v
+--------------------------------------------------------------------------------+
|                            skai-edge-native service                            |
|                                                                                |
|  +----------------------+       +-----------------------------+                |
|  | Boost.Asio / Beast   |<----->| Application / State Model   |                |
|  | REST + WS + WHEP     |       +-----------------------------+                |
|  +----------+-----------+                      ^                               |
|             |                                  | events/status                 |
|             v                                  |                               |
|  +----------------------+                      |                               |
|  | Vanilla Web UI       |                      |                               |
|  +----------------------+                      |                               |
|                                                                                |
|  RTSP URL                                                                      |
|          |                                                                     |
|          v                                                                     |
|  +----------------------+                                                      |
|  | RtspSource           |                                                      |
|  | GStreamer ingest     |                                                      |
|  +----------+-----------+                                                      |
|             | raw frames                                                       |
|             v                                                                  |
|  +----------------------+                                                      |
|  | Bounded Frame Queue  |                                                      |
|  +----------+-----------+                                                      |
|             |                                                                  |
|             v                                                                  |
|  +----------------------+                                                      |
|  | TensorRT Detector    |                                                      |
|  | CUDA preprocessing   |                                                      |
|  +----------+-----------+                                                      |
|             | detections + annotated frames                                    |
|       +-----+-----------------------------+                                    |
|       |                                   |                                    |
|       v                                   v                                    |
| +-------------+                 +----------------------+                       |
| | AlertManager|                 | GStreamer H.264      |                       |
| +------+------+                 | encode / recording   |                       |
|        |                        +----+------------+----+                       |
|        |                             |            |                            |
|        |                             |            +----> MP4 recording         |
|        |                             |                                         |
|        |                             v                                         |
|        |                     encoded H.264 access units                        |
|        |                             |                                         |
|        |                             v                                         |
|        |                     +-------------------+                             |
|        |                     | libdatachannel    |                             |
|        |                     | DTLS/SRTP/RTP     |                             |
|        |                     +---------+---------+                             |
|        |                               |                                       |
|        |                               v                                       |
|        |                          +----------+                                 |
|        |                          | skai-ice |                                 |
|        |                          | STUN/ICE |                                 |
|        |                          +-----+----+                                 |
|        |                                |                                      |
|        |                                +-----------> Browser WebRTC           |
|        |                                                                       |
|        +----> SQLite metadata + JPG snapshot                                   |
|                                                                                |
|  +----------------------+                                                      |
|  | GPS Source           |                                                      |
|  | fixed: Taipei 101    |                                                      |
|  +----------+-----------+                                                      |
|             |                                                                  |
|             +---------------------> Application State                          |
+--------------------------------------------------------------------------------+
```

WebRTC ownership is intentionally split:

```text
Boost.Beast     -> WHEP HTTP signaling/session lifecycle
GStreamer       -> H.264 production
libdatachannel  -> PeerConnection, DTLS, SRTP, RTP
skai-ice        -> STUN + ICE connectivity
Browser         -> native RTCPeerConnection
```

The first WebRTC target is LAN deployment. `skai-edge-native` must not silently fall back to libjuice or introduce a second ICE stack.

---

## 4. Core Design Principles

### 4.1 One process first

Start with one executable:

```text
skai-edge
```

Internal modules should be ordinary C++ classes.

Only split processes later if there is a demonstrated reliability or security reason.

### 4.2 Explicit module boundaries

Suggested interfaces:

```cpp
class RtspSource;
class Detector;
class VideoEncoder;
class Recorder;
class GpsSource;
class AlertManager;
class AlertRepository;
class Database;
class WebServer;
class WebRtcManager;
class WebRtcSession;
class Application;
```

A module should not directly know about unrelated modules.

For example, `Detector` should not issue HTTP responses and `WebServer` should not call TensorRT APIs.

### 4.3 Bounded queues

Video is a real-time stream. Never allow unbounded frame accumulation.

Use bounded producer/consumer queues:

```text
capture thread
    |
    v
[ bounded queue ]
    |
    v
inference thread
```

When inference is slower than capture, prefer dropping stale frames over increasing latency indefinitely.

Initial policy:

```text
queue size = 2 or 3
drop oldest frame when full
```

### 4.4 Strong data models instead of ROS messages

Example:

```cpp
struct Frame {
    uint64_t sequence;
    std::chrono::steady_clock::time_point timestamp;
    int width;
    int height;
    int stride;
    // image ownership
};

struct Detection {
    int class_id;
    float confidence;
    float x1;
    float y1;
    float x2;
    float y2;
};

struct DetectionResult {
    uint64_t frame_sequence;
    std::vector<Detection> detections;
};

struct GpsFix {
    double latitude;
    double longitude;
    double altitude_m;
    float hdop;
    int used_satellites;
    int visible_satellites;
    bool valid;
};

struct AlertEvent {
    std::string id;
    std::chrono::system_clock::time_point timestamp;
    std::vector<Detection> detections;
    std::optional<GpsFix> gps;
    std::string snapshot_path;
};
```

Do not overload unrelated fields as was sometimes necessary when using generic ROS messages.

### 4.5 SQLite for durable local metadata

Use SQLite for structured local metadata that needs queryability and durability.

Initial SQLite responsibilities:

```text
alerts
detections
recordings (later, if useful)
```

Do not store image/video payloads as SQLite BLOBs.

Store media on disk and keep only paths plus metadata in SQLite.

Example:

```text
/var/lib/skai-edge/
├── skai-edge.db
├── alerts/
│   └── 2026-09-15/
│       └── <event-id>.jpg
└── recordings/
    └── recording_YYYYMMDD_HHMMSS.mp4
```

Database access must go through repository classes rather than allowing arbitrary modules to call `sqlite3_*` directly.

Initial rule:

```text
AlertManager
    ↓
AlertRepository
    ↓
SQLite
```

Prefer a single serialized write path. Do not let capture, inference, HTTP, and GPS threads all freely share one writable SQLite connection.

### 4.6 Separate control plane and media plane

Control plane:

```text
Boost.Beast HTTP
Boost.Beast WebSocket
Boost.Beast WHEP endpoint
JSON / SDP
```

Media plane:

```text
GStreamer RTSP ingest / H.264 encode / recording
libdatachannel DTLS / SRTP / RTP
skai-ice STUN / ICE
optional RTSP
```

Do not send video frames as JSON or ordinary REST responses.

### 4.7 One WebRTC/ICE stack

Use exactly this browser-video stack:

```text
GStreamer H.264
      ↓
libdatachannel
      ↓
skai-ice
      ↓
Browser
```

`skai-ice` exposes a libjuice-compatible C ABI so libdatachannel can use it through `USE_SYSTEM_JUICE=ON`.

Rules:

- `skai-ice` remains an independent repository and is consumed as a pinned dependency.
- disable the standalone `skai-ice-server` when embedding the library
- do not use `cpp-httplib` in `skai-edge-native`
- do not use GStreamer `webrtcbin`
- do not link libdatachannel's normal libjuice backend by mistake
- configure the skai-ice host-interface whitelist before the first PeerConnection/`juice_create`
- if libdatachannel is upgraded, verify the libjuice ABI/header expected by `skai-ice`
- first release is host-candidate/LAN-only
- NAT traversal improvements belong in `skai-ice`, not in a parallel implementation here

### 4.8 Test-first development

This project is developed test-first. Every step follows:

```text
write failing tests for the new behavior (red)
   ↓
implement until the tests pass (green)
   ↓
refactor with the tests still passing
```

Rules:

- The test framework exists from Step 1; no step is allowed to postpone its tests to a later "testing phase".
- Each step's Acceptance list is turned into automated tests wherever it can be automated. Items that cannot (for example "Chrome on another LAN machine receives video") are written as a manual verification checklist in the pull request description.
- Framework: GoogleTest driven by CTest (`find_package(GTest REQUIRED)`, `gtest_discover_tests`). Ubuntu 22.04 / JetPack 6 provides it as `libgtest-dev`.
- Tests that need Jetson hardware (NVDEC, TensorRT, CUDA) or a live network fixture carry CTest labels (`jetson`, `rtsp`, `webrtc`) so x86 Linux can run the rest with `ctest -LE jetson`.
- Keep hardware-bound code thin and pure logic separate, so that the logic is testable on x86 without a GPU. Examples: RTSP reconnect/stall state machine, YOLO postprocessing/NMS, alert rules, GPS config validation, WHEP/SDP validation, JSON DTOs.
- Test fixtures (for example the local RTSP test server) live under `tests/` and are never linked into `skai-edge`. They must not create a back door for non-RTSP input into the application.

### 4.9 Step size and splitting

Each numbered step is a deliverable, not a fixed-size change. During
implementation, count the actual changed code lines. Do not split solely on
an estimate. If implementation and test code exceeds 800 changed lines, split
it before submission into consecutive substeps: `step-N-a`, `step-N-b`,
`step-N-c`, and so
on; title them `Step N-a`, `Step N-b`, etc. Keep each substep at or below 800
changed code lines. Count code under `include/`, `src/`, `tests/`, and `cmake/` (including
`CMakeLists.txt`); exclude documentation, assets, and generated files. Give each
substep a clear behavior and its own failing tests, then keep all earlier tests
passing. Divide the parent step's acceptance criteria across the substeps and
verify the complete acceptance list in the final substep. Keep the original
step number for traceability in commits and pull requests.

---

# Phase 0 — Clean Repository Foundation

Goal: establish a new repository with no dependency on the old ROS 2 implementation.

## Step 1 — Repository skeleton

Create:

```text
skai-edge-native/
├── CMakeLists.txt
├── cmake/
├── config/
├── include/skai/
├── src/
├── tests/
├── web/
├── scripts/
├── systemd/
├── docs/
├── models/
├── third_party/
└── README.md
```

Initial executable:

```text
src/main.cpp
```

Requirements:

- CMake build works on x86 Linux
- CMake build works on Jetson
- GoogleTest + CTest wired up (`enable_testing()`, `tests/unit/`, `tests/integration/`)
- `--help`
- `--version`
- clean shutdown on SIGINT / SIGTERM

Tests written first:

- command-line parsing: `--help`, `--version`, unknown option → non-zero exit with usage message
- integration test: launch `skai-edge`, send SIGINT and SIGTERM, assert exit code 0 within a timeout

Acceptance:

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/skai-edge --version
```

---

## Step 2 — Configuration and logging

Add a configuration layer.

Suggested config:

```yaml
video:
  rtsp_url: "rtsp://192.168.1.100:554/stream"
  transport: tcp
  latency_ms: 100
  reconnect_delay_ms: 1000
  stall_timeout_ms: 3000

detector:
  engine: models/yolo11s.engine
  confidence: 0.35
  nms: 0.45

web:
  bind: 0.0.0.0
  port: 8080

recording:
  enabled: true
  directory: recordings
  segment_seconds: 300

gps:
  enabled: true
  source: fixed            # first version: fixed position, no GPS hardware
  latitude: 25.033964      # Taipei 101
  longitude: 121.564468
  altitude_m: 10.0

webrtc:
  enabled: true
  max_peers: 2
  host_interfaces:
    - eth0
    - wlan0
  ice_log_verbosity: 1
```

Implementation may use YAML, TOML, or JSON, but configuration parsing must remain isolated from application logic.

Add structured logging with levels:

```text
trace
debug
info
warning
error
```

Acceptance:

- invalid configuration gives a useful error
- configuration is validated before pipeline startup
- logs contain timestamps and module names

---

# Phase 1 — Application Runtime

Goal: build the internal runtime that replaces ROS 2 lifecycle/topic behavior.

## Step 3 — Application lifecycle

Implement:

```cpp
class Application {
public:
    bool initialize();
    bool start();
    void stop();
    void wait();
};
```

Startup order:

```text
load config
  ↓
initialize web server
  ↓
initialize detector
  ↓
initialize video pipeline
  ↓
initialize GPS
  ↓
start worker threads
```

Shutdown order should be deterministic and reverse initialization dependencies.

Acceptance:

- repeated start/stop testing does not deadlock
- SIGTERM exits cleanly
- all worker threads are joined

---

## Step 4 — BoundedQueue and event primitives

Implement a small reusable bounded queue.

Required behavior:

- fixed capacity
- blocking or timed pop
- drop-oldest policy
- shutdown notification
- statistics:
  - pushed
  - popped
  - dropped
  - high-water mark

Do NOT implement a generic ROS-like pub/sub framework.

Acceptance:

- unit tests for concurrent producer/consumer behavior
- clean shutdown while consumer is blocked
- concurrency tests pass under ThreadSanitizer (`-fsanitize=thread` build)

---

# Phase 2 — RTSP Ingest with GStreamer

Goal: implement the only supported video input path: an RTSP URL.

There is no generic video-source abstraction in this repository.

The application does NOT support:

```text
USB / V4L2 camera
MIPI CSI / Argus camera
local video file
webcam
arbitrary pluggable capture source
```

The runtime owns exactly one input module:

```cpp
class RtspSource;
```

Its job is to turn an RTSP stream into decoded frames suitable for TensorRT.

## Step 5 — GStreamer runtime wrapper

Create minimal RAII wrappers for:

- `GstElement`
- `GstBus`
- pipeline state
- startup/shutdown
- error reporting

Initialize GStreamer once from `main()`.

Add the RTSP test fixture here, because Step 6 and Step 7 are tested against it:

```text
tests/fixtures/rtsp_test_server
  gst-rtsp-server (libgstrtspserver-1.0-dev)
  videotestsrc → H.264 (and H.265) → rtsp://127.0.0.1:<ephemeral-port>/test
  can be stopped/restarted/stalled on demand by the test
```

The fixture is test-only and is not linked into `skai-edge`.

Acceptance:

- a simple RTSP test pipeline can enter PLAYING
- EOS and ERROR bus messages are handled
- state transitions are logged
- no GStreamer resource leak on shutdown

---

## Step 6 — RtspSource

Implement:

```cpp
class RtspSource;
```

Responsibilities:

- connect to exactly one configured RTSP URL
- support RTSP authentication when credentials are embedded or configured
- configure TCP or UDP transport
- configure GStreamer latency
- receive RTP
- depayload H.264 or H.265
- parse elementary stream
- use Jetson hardware decode when available
- convert decoded output into the frame format expected by inference
- expose frames through `appsink`
- attach frame sequence and timestamps
- report source health

Typical H.264 path:

```text
rtspsrc
  ↓
RTP / jitter handling
  ↓
rtph264depay
  ↓
h264parse
  ↓
nvv4l2decoder
  ↓
nvvidconv / videoconvert as required
  ↓
appsink
```

Typical H.265 path:

```text
rtspsrc
  ↓
RTP / jitter handling
  ↓
rtph265depay
  ↓
h265parse
  ↓
nvv4l2decoder
  ↓
nvvidconv / videoconvert as required
  ↓
appsink
```

Note: `nvv4l2decoder` is used only as NVIDIA's GStreamer hardware decoder; its name does not imply V4L2 camera-input support.

Do not create:

```text
VideoSourceType
V4l2Source
CsiSource
FileSource
UsbCameraSource
```

Acceptance:

- connect to a configured H.264 RTSP stream
- connect to a configured H.265 RTSP stream if H.265 support is enabled
- frames arrive at the bounded inference queue
- negotiated codec/resolution/FPS are visible in diagnostics
- application fails clearly when the URL is invalid
- clean shutdown releases the RTSP pipeline

---

## Step 7 — RTSP recovery, latency, and diagnostics

Make RTSP behavior production-oriented.

Handle:

```text
RTSP server unavailable at startup
RTSP endpoint/server restart
temporary network loss
RTP packet loss
pipeline ERROR
pipeline EOS
stream stall without immediate socket failure
codec/resolution renegotiation where practical
```

Required recovery model:

```text
CONNECTED
   ↓
DEGRADED / STALLED
   ↓
RECONNECTING
   ↓
CONNECTED
```

Add:

- reconnect delay/backoff
- stall timeout based on frame freshness
- pipeline rebuild after unrecoverable GStreamer error
- last-frame timestamp
- reconnect counter
- packet/jitter diagnostics where available
- configurable RTSP transport:
  - TCP
  - UDP
- configurable GStreamer latency

Expose runtime metrics:

```json
{
  "url_configured": true,
  "connected": true,
  "transport": "tcp",
  "codec": "H264",
  "width": 1920,
  "height": 1080,
  "fps_in": 29.8,
  "frames_received": 30291,
  "frames_dropped": 38,
  "last_frame_age_ms": 12,
  "reconnect_count": 2
}
```

Add CLI diagnostic mode:

```bash
skai-edge --rtsp-test
```

No inference is required in this mode.

Important rule:

> A stale or reconnecting RTSP input must not deadlock the rest of the application.

Acceptance:

- starting while the RTSP endpoint is offline enters reconnect mode
- restarting the RTSP endpoint/server is recovered automatically
- a stalled stream is detected even when no immediate GStreamer ERROR is emitted
- reconnect attempts do not leak pipelines, threads, or file descriptors
- the application can be stopped cleanly while reconnecting
- each item above is an automated test driving Step 5's RTSP test fixture (offline at start, restart, stall); the CONNECTED/STALLED/RECONNECTING state machine is also unit-tested without GStreamer

---


# Phase 3 — TensorRT Detection

Goal: add a production-quality inference path independently from web and recording.

## Step 8 — TensorRT engine loader

Implement:

```cpp
class TensorRtEngine;
```

Responsibilities:

- deserialize `.engine`
- validate input/output bindings
- allocate GPU buffers
- own CUDA stream
- report engine metadata

Acceptance:

- engine loads on Jetson
- clear errors for incompatible engine
- no hidden global TensorRT state

---

## Step 9 — CUDA preprocessing

Implement GPU preprocessing:

```text
decoded RTSP frame
   ↓
resize / letterbox
   ↓
BGR → RGB
   ↓
normalize
   ↓
NCHW
   ↓
TensorRT input
```

Keep CPU reference implementation for correctness testing.

Acceptance:

- CUDA and reference preprocessing agree within tolerance
- measure preprocessing latency

---

## Step 10 — YOLO11 inference and postprocessing

Implement:

- TensorRT inference
- output decode
- confidence filtering
- per-class NMS
- coordinate restoration

Output:

```cpp
DetectionResult
```

Acceptance:

- compare against known reference images
- deterministic regression test
- configurable confidence/NMS thresholds
- expose inference timing

---

## Step 11 — Annotation

Draw:

- bounding boxes
- class name
- confidence
- optional FPS/inference timing

Start with CPU/OpenCV drawing.

CUDA drawing is explicitly deferred until profiling shows it is necessary.

Acceptance:

- annotated frames are produced without modifying inference correctness
- annotation can be disabled

---

# Phase 4 — Boost.Beast Web Backend

Goal: replace Flask/FastAPI completely with a native C++ control plane.

## Step 12 — HTTP server foundation

Implement using:

```text
Boost.Asio
Boost.Beast
```

Start with:

```text
GET /health
GET /api/v1/status
```

Example:

```json
{
  "status": "running",
  "uptime_s": 812,
  "video": {
    "fps": 29.9
  },
  "detector": {
    "fps": 18.4,
    "last_inference_ms": 43.1
  }
}
```

Requirements:

- asynchronous I/O
- graceful shutdown
- request size limit (baseline: fixed safe default for header and body)
- request timeout (baseline: fixed read/write timeout)
- clear routing layer
- no framework above Beast

Scope split with Step 40: this step sets baseline limits so the server is never unbounded
from day one. Step 40 audits every entry point added later (WebSocket, WHEP, static
files), makes limits configurable, and adds the remaining hardening.

Suggested structure:

```text
src/web/
├── http_server.cpp
├── http_session.cpp
├── router.cpp
├── websocket_session.cpp
└── static_file_handler.cpp
```

---

## Step 13 — REST API surface

Add:

```text
GET  /api/v1/status
GET  /api/v1/config
GET  /api/v1/detections/latest
GET  /api/v1/gps
GET  /api/v1/alerts
GET  /api/v1/alerts/{id}
GET  /api/v1/alerts?limit=100
GET  /api/v1/alerts?class=person
GET  /api/v1/alerts?from=<timestamp>&to=<timestamp>
GET  /api/v1/recordings
POST /api/v1/recording/start
POST /api/v1/recording/stop
POST /api/v1/detector/enable
POST /api/v1/detector/disable
```

Do not expose arbitrary internal C++ state directly.

Use explicit DTOs.

---

## Step 14 — WebSocket event channel

Endpoint:

```text
GET /ws
```

Push:

```text
status
detection
gps
alert
recording
system_error
```

Example:

```json
{
  "type": "alert",
  "timestamp": "2026-09-15T10:30:21.412+08:00",
  "data": {
    "class": "person",
    "confidence": 0.91
  }
}
```

Requirements:

- multiple clients
- slow-client protection
- bounded outgoing queue per client
- ping/pong keepalive
- clean disconnect

---

# Phase 5 — Minimal Browser UI

Goal: provide a useful monitoring frontend with zero JavaScript framework dependencies.

## Step 15 — Static frontend

Serve from Beast:

```text
/
├── index.html
├── app.js
└── style.css
```

UI sections:

```text
System status
RTSP status
Detector status
Latest detections
GPS status
Recent alerts
Recording controls
Live video placeholder
```

Use:

- `fetch()`
- `WebSocket`
- DOM APIs
- `<video>`

Do NOT use:

- React
- Vue
- Angular
- jQuery
- frontend build toolchain

The frontend should be editable and runnable without npm.

The static file handler is confined to the web root from its first version, not
deferred to Step 40.

Tests written first:

- `GET /` serves `index.html`; `GET /app.js` and `GET /style.css` return the correct `Content-Type`
- unknown file → 404
- path traversal is rejected and never escapes the web root:
  - `GET /../config/config.example.yaml`
  - percent-encoded `GET /%2e%2e/%2e%2e/etc/passwd`
  - `GET /..%2f..%2fetc/passwd`
- a symlink inside the web root that points outside it is not followed
- requests for directories do not produce a directory listing

Implementation note: decode the target first, then resolve it with
`std::filesystem::weakly_canonical` and verify that the result is still inside the
canonical web root. Do not rely on string matching for `..`.

---

## Step 16 — Live status UI

Connect:

```text
REST -> initial page state
WebSocket -> incremental updates
```

Requirements:

- reconnect WebSocket automatically
- clearly show disconnected state
- no page refresh required
- keep JavaScript small and understandable

---

# Phase 6 — GPS

Goal: provide GPS data without ROS messages.

**First version: fixed position at Taipei 101. No GPS hardware is read.**

## Step 17 — Fixed-position GpsSource

Implement a concrete class (no virtual interface yet — see rule 24):

```cpp
class GpsSource {
public:
    explicit GpsSource(const GpsConfig& config);
    std::optional<GpsFix> latest() const;
};
```

`latest()` returns the configured fixed position. Default configuration is
Taipei 101:

```yaml
gps:
  enabled: true
  source: fixed            # only supported value in the first version
  latitude: 25.033964
  longitude: 121.564468
  altitude_m: 10.0         # approximate ground elevation
```

The fixed fix reports `valid = true`, a nominal `hdop`, and nominal satellite
counts. There is no GPS thread, no serial port, and no network socket.

Every place GPS is shown or stored must say it is fixed, so simulated coordinates
are never mistaken for a real position:

- `GET /api/v1/gps` and the WebSocket `gps` event include `"source": "fixed"`
- the UI labels the position as fixed/simulated
- the `alerts` table records `gps_source` (see Step 18)

Tests written first:

- default config yields Taipei 101 coordinates
- configured latitude/longitude/altitude are returned unchanged
- out-of-range latitude (outside ±90) or longitude (outside ±180) fails config validation
- `gps.enabled: false` makes `latest()` return `std::nullopt`
- `source` values other than `fixed` are rejected with a clear error

Deferred until a real device exists:

```text
Serial UBX/NMEA
MAVLink UDP GPS input (never MAVROS)
GPS receive thread, fix timeout, fix-age tracking
```

When the first real receiver is added, extract a `GpsSource` interface at that
point, with the fixed source kept as the second implementation for tests.

Expose via:

```text
GET /api/v1/gps
WebSocket gps event
```

---

# Phase 7 — SQLite Persistence

Goal: establish durable, queryable local storage before alert persistence is implemented.

## Step 18 — SQLite database foundation

Use the official SQLite C API through:

```cpp
#include <sqlite3.h>
```

CMake:

```cmake
find_package(SQLite3 REQUIRED)

target_link_libraries(skai-edge
    PRIVATE
    SQLite::SQLite3
)
```

Create:

```cpp
class Database;
class AlertRepository;
```

Suggested location:

```text
include/skai/storage/
src/storage/
```

Database file:

```text
/var/lib/skai-edge/skai-edge.db
```

Initial schema:

```sql
CREATE TABLE alerts (
    id TEXT PRIMARY KEY,
    timestamp_ms INTEGER NOT NULL,
    latitude REAL,
    longitude REAL,
    altitude_m REAL,
    gps_valid INTEGER NOT NULL DEFAULT 0,
    gps_source TEXT NOT NULL DEFAULT 'none',  -- 'fixed' | 'none' (real sources later)
    snapshot_path TEXT NOT NULL,
    frame_sequence INTEGER NOT NULL,
    model_version TEXT
);

CREATE TABLE detections (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    alert_id TEXT NOT NULL,
    class_id INTEGER NOT NULL,
    class_name TEXT NOT NULL,
    confidence REAL NOT NULL,
    x1 REAL NOT NULL,
    y1 REAL NOT NULL,
    x2 REAL NOT NULL,
    y2 REAL NOT NULL,
    FOREIGN KEY(alert_id) REFERENCES alerts(id) ON DELETE CASCADE
);

CREATE INDEX idx_alerts_timestamp
ON alerts(timestamp_ms DESC);

CREATE INDEX idx_detections_alert_id
ON detections(alert_id);

CREATE INDEX idx_detections_class_name
ON detections(class_name);
```

Use schema migrations from the beginning.

Example:

```text
schema_version
v1_initial.sql
v2_...
```

Requirements:

- enable foreign keys
- use transactions when inserting an alert and its detections
- prepared statements for runtime queries
- explicit error propagation
- deterministic DB close on shutdown
- WAL mode should be evaluated for concurrent read/write behavior
- DB access should be serialized or otherwise deliberately synchronized

Acceptance:

- database is created automatically
- migration is idempotent
- inserting one alert plus N detections is atomic
- rollback works on failure
- concurrent API reads do not corrupt or block the writer indefinitely
- unit tests run on x86 Linux using a temporary database

---

# Phase 8 — Alert Engine

Goal: generate useful edge events without coupling the detector to the web layer.

## Step 19 — Alert rules

Implement basic rules:

```text
class-based trigger
minimum confidence
cooldown
minimum consecutive frames
ROI filtering
```

Example:

```yaml
alerts:
  - class: person
    confidence: 0.70
    consecutive_frames: 3
    cooldown_seconds: 10
```

Detector produces detections.

AlertManager decides whether those detections are events.

---

## Step 20 — Snapshot and alert persistence

For every accepted alert:

```text
AlertManager
    |
    +----> write JPG snapshot
    |
    +----> AlertRepository
               |
               +----> alerts row
               +----> detections rows
```

Snapshot layout:

```text
/var/lib/skai-edge/alerts/
└── 2026-09-15/
    └── <event-id>.jpg
```

SQLite stores:

- alert ID
- timestamp
- GPS fields, including `gps_source`
- snapshot path
- frame sequence
- model version
- all associated detections

Do not create one JSON metadata file per alert.

The image remains a normal file on disk; SQLite stores only its path.

`AlertRepository` should provide at least:

```cpp
bool insert(const AlertEvent& event);

std::optional<AlertEvent> find_by_id(
    const std::string& id);

std::vector<AlertEvent> find_recent(
    std::size_t limit);

std::vector<AlertEvent> find_by_time_range(
    TimePoint from,
    TimePoint to);

std::vector<AlertEvent> find_by_class(
    std::string_view class_name,
    std::size_t limit);
```

Acceptance:

- snapshot write and DB insert failure are both handled explicitly
- DB row never points to a nonexistent successful snapshot because of partial workflow
- one alert can contain multiple detections
- alert queries return detections with the parent alert
- oldest alert cleanup can delete both DB metadata and snapshot file safely

---

# Phase 9 — Recording

Goal: preserve the useful recording functionality while keeping media handling inside GStreamer.

## Step 21 — H.264 encoder pipeline

Annotated frame:

```text
appsrc
  ↓
videoconvert
  ↓
x264enc
  ↓
h264parse
```

Profile Jetson CPU usage carefully.

Do not assume hardware NVENC is available on the target device.

Expose encoder metrics.

The encoded output must be reusable by both recording and WebRTC without requiring
the detector to know either consumer.

Target encoded format for the WebRTC branch:

```text
H.264
stream-format=byte-stream
alignment=au
```

Prefer a bounded encoded-access-unit queue between GStreamer and the WebRTC layer.
A slow browser must never stall capture or inference.

---

## Step 22 — Segmented MP4 recording

Pipeline:

```text
H.264
  ↓
splitmuxsink
  ↓
recording_YYYYMMDD_HHMMSS.mp4
```

Features:

- segment duration
- maximum storage quota
- start/stop API
- recording state
- disk-space check
- graceful file finalization

---

# Phase 10 — Browser Live Video with skai-ice

Goal: integrate the existing `skai-ice` repository as the only ICE implementation
and deliver the GStreamer-produced H.264 stream to a browser through libdatachannel.

This phase does NOT use:

- GStreamer `webrtcbin`
- MediaMTX as a required WebRTC hop
- `cpp-httplib`
- libjuice as a hidden fallback
- TURN / srflx candidates in the first version

The first target is:

```text
Browser on LAN
      |
      | WHEP offer/answer over HTTP
      v
Boost.Beast
      |
      v
WebRtcManager
      |
      +---- libdatachannel: PeerConnection / DTLS / SRTP / RTP
      |
      +---- skai-ice: STUN / ICE
      |
      v
H.264 media to browser
```

## Step 23 — Integrate skai-ice and libdatachannel

Add pinned dependencies:

```text
third_party/
├── skai-ice/
└── libdatachannel/
```

Use git submodules or another reproducible pinning mechanism. Do not track floating
branch heads in production builds.

The parent CMake integration should follow the contract exposed by `skai-ice`:

```cmake
set(SKAI_ICE_BUILD_SERVICE OFF CACHE BOOL "" FORCE)
set(SKAI_ICE_BUILD_TESTS OFF CACHE BOOL "" FORCE)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
set(TEST_APPS OFF CACHE BOOL "" FORCE)
set(USE_SYSTEM_JUICE ON CACHE BOOL "" FORCE)

add_subdirectory(third_party/skai-ice)
add_subdirectory(third_party/libdatachannel)
```

Expected ownership:

```text
LibJuice::LibJuice
       |
       +---- alias provided by skai-ice

LibDataChannel::LibDataChannel
       |
       +---- resolves ICE calls through skai-ice
```

Do not build or embed the `skai-ice-server` daemon. Its `cpp-httplib` HTTP service
is not part of this application.

At startup, before the first PeerConnection is created:

```cpp
skai_ice_set_host_interfaces(...);
skai_ice_set_log_verbosity(...);
```

Requirements:

- dependency revisions are pinned
- build fails clearly if the expected dependency targets are missing
- no duplicate libjuice implementation is linked
- `skai-ice` standalone tests continue to live in its own repository
- this repository adds an integration smoke test proving libdatachannel is using
  the `skai-ice` ABI
- document the compatibility rule between the pinned libdatachannel revision and
  the vendored `juice.h` ABI used by `skai-ice`

Acceptance:

- `skai-edge-native` builds with `skai-ice` embedded as a library
- `SKAI_ICE_BUILD_SERVICE=OFF`
- a minimal PeerConnection reaches ICE gathering using `skai-ice`
- selected host interfaces appear in ICE diagnostics
- there is no `cpp-httplib` runtime dependency

---

## Step 24 — Boost.Beast WHEP session API

Implement WHEP signaling with Boost.Beast.

Initial API:

```text
POST   /api/v1/webrtc/whep
DELETE /api/v1/webrtc/sessions/{session_id}
```

`POST`:

```text
Browser SDP offer
      ↓
Boost.Beast route
      ↓
WebRtcManager::create_session()
      ↓
libdatachannel PeerConnection
      ↓
SDP answer
```

Return:

```text
201 Created
Content-Type: application/sdp
Location: /api/v1/webrtc/sessions/<session-id>
```

Initial scope intentionally does not implement WHEP PATCH/Trickle ICE because
`skai-ice` is host-candidate/LAN-oriented and does not provide full Trickle ICE.

Create:

```cpp
class WebRtcManager {
public:
    CreateSessionResult create_session(std::string_view offer_sdp);
    bool close_session(std::string_view session_id);
};

class WebRtcSession {
public:
    std::string id() const;
    void close();
};
```

Responsibilities:

```text
Boost.Beast
    |
    +---- parses HTTP only
    |
    v
WebRtcManager
    |
    +---- owns session map
    +---- creates/destroys PeerConnection
    +---- returns SDP answer
```

Do not put libdatachannel calls directly inside generic HTTP session code.

Acceptance:

- malformed SDP returns a useful 4xx response
- session IDs are unique and non-guess-dependent enough for LAN use
- DELETE closes and releases the PeerConnection
- stale sessions are cleaned up
- application shutdown closes all peers deterministically
- no WebSocket is required for WHEP offer/answer

---

## Step 25 — H.264 media delivery to libdatachannel

Connect the existing GStreamer H.264 output to libdatachannel.

Target:

```text
Annotated frame
      ↓
GStreamer appsrc
      ↓
x264enc
      ↓
h264parse
      ↓
H.264 access units
      ↓
bounded encoded queue
      ↓
WebRtcSession
      ↓
libdatachannel media track
      ↓
DTLS / SRTP / RTP
      ↓
skai-ice connectivity
      ↓
Browser <video>
```

Requirements:

- H.264 profile/level must be compatible with target browsers
- SPS/PPS availability must be handled for newly connected peers
- keyframe behavior must allow a new peer to start decoding promptly
- encoded-media queues are bounded
- one slow/disconnected peer does not block encoder, detector, or other peers
- peer lifecycle is independent from recording lifecycle

Vanilla browser code uses:

```javascript
const pc = new RTCPeerConnection();
pc.addTransceiver("video", { direction: "recvonly" });

const offer = await pc.createOffer();
await pc.setLocalDescription(offer);

const response = await fetch("/api/v1/webrtc/whep", {
  method: "POST",
  headers: { "Content-Type": "application/sdp" },
  body: offer.sdp
});

const answer = await response.text();
await pc.setRemoteDescription({
  type: "answer",
  sdp: answer
});
```

No React/Vue/npm build chain is needed.

Acceptance:

- Chrome on another machine on the same LAN receives video
- Firefox is tested if practical
- browser reconnect works
- video continues recording when the browser disconnects
- browser disconnect does not affect inference
- end-to-end media path contains no MediaMTX requirement

---

## Step 26 — WebRTC/ICE hardening and diagnostics

Expose WebRTC state through:

```text
GET /api/v1/status
GET /api/v1/metrics
WebSocket status events
```

Track:

```text
active peers
peer state
ICE state
selected/local interface
connection age
bytes/packets sent
media queue drops
keyframe requests/events where available
session close reason
```

Add:

- connection timeout
- peer cleanup
- maximum peer count
- per-peer bounded queues
- clean service shutdown
- ICE logging configuration
- explicit LAN-only status in diagnostics
- error propagation from libdatachannel and skai-ice

Important architectural rule:

> If Internet/NAT traversal becomes a requirement, extend and test `skai-ice`
> first. Do not add a second TURN/libjuice ICE path only inside
> `skai-edge-native`.

Acceptance:

- two or more LAN viewers are tested if the configured limit permits
- disconnect/reconnect does not leak sessions
- repeated offer/delete cycles do not grow memory indefinitely
- diagnostics identify whether failure occurred in signaling, PeerConnection,
  ICE, or media delivery

---

# Phase 11 — Optional RTSP Output

## Step 27 — RTSP output

Implement only if a non-browser RTSP consumer is still required.

Possible implementations:

- GStreamer `gst-rtsp-server`
- a separately deployed MediaMTX process

RTSP is not part of the browser WebRTC path.

Preferred relationship:

```text
                    +----> MP4 recording
GStreamer H.264 ----+----> libdatachannel / WebRTC
                    +----> optional RTSP
```

Do not introduce RTSP merely as an intermediate hop between this service and
libdatachannel.

---

# Phase 12 — Observability and Diagnostics

## Step 28 — Metrics

Track:

```text
RTSP ingest FPS
inference FPS
inference latency
queue drops
encoder FPS
encoded-media queue drops
WebSocket clients
WebRTC clients
WebRTC peer state
ICE state
GPS source (fixed) / GPS age once real hardware exists
alert count
memory usage
CPU usage
GPU utilization if available
disk free space
```

Expose:

```text
GET /api/v1/status
GET /api/v1/metrics
```

Prometheus format can be added later if useful.

---

## Step 29 — Diagnostic page

Add a simple `/diagnostics` page showing:

- RTSP source status
- queue depth
- TensorRT timing
- recording state
- GPS source and position (freshness once real hardware exists)
- active WebSocket sessions
- active WebRTC peers
- ICE state
- selected host interface/candidate information safe to display locally
- recent errors

Still vanilla JavaScript.

---

# Phase 13 — Reliability

## Step 30 — Failure recovery

Test and handle:

- RTSP endpoint unavailable
- RTSP disconnect / reconnect
- RTSP stall
- RTP packet loss / jitter degradation
- malformed frame
- TensorRT failure
- encoder failure
- disk full
- GPS timeout (deferred until real GPS hardware; the fixed source cannot time out)
- browser disconnect
- WebSocket slow client
- malformed WHEP offer
- libdatachannel PeerConnection failure
- skai-ice gathering/check failure
- WebRTC media queue backpressure

Define which failures are:

```text
recoverable
restart-module
fatal-process
```

Avoid terminating the whole process for a recoverable media-source or peer error.

---

## Step 31 — Watchdog and health model

Internal states:

```text
STARTING
RUNNING
DEGRADED
STOPPING
FAILED
```

Each component reports health independently:

```text
video_source
detector
encoder
recorder
gps
web
webrtc
database
```

`/health` should reflect actual service health rather than merely returning HTTP 200.

A single failed browser peer should not mark the whole service failed.

---

# Phase 14 — Test Hardening

## Step 32 — Test coverage audit and sanitizer builds

The test framework has existed since Step 1 and every step shipped its own tests (see 4.8).
This step does not introduce testing; it audits and closes gaps.

Add:

- coverage report (`--coverage` + gcovr or lcov) and a minimum threshold for pure-logic modules
- AddressSanitizer / UndefinedBehaviorSanitizer build preset running the full x86 suite
- ThreadSanitizer build preset for queue, lifecycle, WebSocket, and WebRTC session code
- a single command that runs the x86-safe suite: `ctest -LE jetson`

Verify each of these areas already has tests; add any that are missing:

- configuration
- bounded queue
- routing
- static-file root confinement / path traversal
- JSON serialization
- WHEP route validation
- WebRTC session registry
- alert rules
- SQLite migrations
- AlertRepository CRUD/query behavior
- transaction rollback
- GPS fixed-source config validation
- YOLO postprocessing
- state model

Tests should run on ordinary x86 Linux when GPU hardware is not required.

Do not duplicate `skai-ice`'s STUN/ICE protocol unit tests here. Test the integration
boundary instead.

---

## Step 33 — Integration tests

Compose the per-module integration tests written since Step 5 into end-to-end
tests of the main pipeline.

The application must still receive video through RTSP. For deterministic CI,
Step 5's RTSP test fixture publishes a known stream through a local RTSP server;
the application itself must not gain a file-input code path.

```text
fixed test clip / generated frames
   ↓
test RTSP server
   ↓
RTSP URL
   ↓
GStreamer
   ↓
detector
   ↓
alert
   ↓
recording
   ↓
HTTP status
```

Add a separate WebRTC integration fixture:

```text
SDP offer
   ↓
Beast WHEP route
   ↓
libdatachannel
   ↓
skai-ice
   ↓
PeerConnection state / media send smoke test
```

Use a fixed test video and deterministic expected detections where possible.

---

## Step 34 — Jetson smoke tests

Script:

```bash
scripts/smoke_test_jetson.sh
```

Verify:

- configured RTSP stream connects
- TensorRT engine loads
- inference starts
- web API responds
- browser UI loads
- recording works
- SQLite alert persistence works
- WHEP endpoint returns a valid SDP answer
- LAN browser receives live video
- `skai-ice` host-interface selection is visible in diagnostics
- clean shutdown works

---

# Phase 15 — Packaging and Deployment

## Step 35 — systemd service

Create:

```text
systemd/skai-edge.service
```

Requirements:

```text
Restart=on-failure
```

Log through stdout/stderr or journald.

Provide:

```bash
systemctl start skai-edge
systemctl stop skai-edge
systemctl restart skai-edge
systemctl status skai-edge
journalctl -u skai-edge
```

---

## Step 36 — install/package flow

Support:

```bash
cmake --install
```

Install:

```text
/usr/local/bin/skai-edge
/etc/skai-edge/config.yaml
/usr/local/share/skai-edge/web/
/var/lib/skai-edge/skai-edge.db
/var/lib/skai-edge/recordings/
/var/lib/skai-edge/alerts/
```

Document the exact pinned revisions of:

```text
skai-ice
libdatachannel
```

Build instructions must include recursive dependency checkout when git submodules
are used.

Later add `.deb` packaging if deployment becomes frequent.

---

# Phase 16 — Performance Optimization

Optimization comes only after the end-to-end system is measurable.

## Step 37 — Profiling baseline

Record:

```text
RTSP ingest FPS
RTSP ingest/decode CPU
preprocess ms
TensorRT ms
postprocess ms
annotation ms
encode CPU
encoded queue latency
WebRTC media latency
end-to-end browser latency
RAM
GPU utilization
```

Create a baseline on Orin Nano.

---

## Step 38 — Zero-copy investigation

Investigate reducing:

```text
NVMM
  ↓
CPU BGR copy
  ↓
CUDA upload
```

Possible future direction:

```text
NVMM / DMA-BUF
   ↓
CUDA
   ↓
TensorRT
```

Only implement after measuring the copy cost.

Do not let zero-copy work couple TensorRT internals directly to Beast or
libdatachannel.

---

## Step 39 — Encoder optimization

If `x264enc` consumes too much CPU:

- reduce output resolution
- reduce frame rate
- tune preset
- tune bitrate
- encode only when there is a recording or WebRTC/RTSP consumer
- share encoded output when compatible
- separate recording and live-stream requirements when they need different
  resolution/bitrate

Do not optimize blindly.

---

# Phase 17 — Security

## Step 40 — Web server and WHEP hardening

Starting from Step 12's baseline, audit every entry point and add:

- configurable request size limits (extends Step 12 baseline)
- SDP body size limit
- path traversal protection (re-verifies Step 15 tests against all static routes, including `/diagnostics`)
- static-file root confinement (re-verifies Step 15)
- WebSocket message size limits
- per-endpoint timeouts (extends Step 12 baseline)
- connection limits
- WebRTC peer limits (verifies Step 26 enforcement)
- safe JSON parsing
- no shell-command API
- predictable cleanup of abandoned WHEP sessions

Each item has a negative test (oversized body, `../` path, slow client, peer flood)
written before its fix.

For LAN-only development, authentication may initially be disabled.

Before exposing the HTTP service outside a trusted LAN, add authentication and TLS.

This does not change the ICE limitation: Internet/NAT traversal is a separate
`skai-ice` capability decision.

---

# Recommended Runtime Thread Model

Initial model:

```text
Main thread
 └─ Application lifecycle

Thread A
 └─ Boost.Asio io_context
    ├─ REST
    ├─ application WebSocket
    └─ WHEP HTTP signaling

Thread B
 └─ GStreamer RTSP ingest / appsink processing

Thread C
 └─ TensorRT inference

Thread D
 └─ GStreamer encode / recording coordination

Thread E
 └─ (reserved) GPS receive/parser — not needed while GPS is a fixed position

Library/internal workers
 └─ libdatachannel / skai-ice callbacks as required
```

Do not create one thread per HTTP or WHEP connection.

Boost.Asio handles network concurrency for the Beast control plane.

Callbacks from libdatachannel/skai-ice must hand work into owned application
state safely rather than mutating unrelated modules directly.

---

# Recommended Internal Data Flow

```text
                           +------------------+
                           | ApplicationState |
                           +--------+---------+
                                    ^
                                    |
              +---------------------+----------------------+
              |                     |                      |
             GPS                  Detector              Recorder
              |                     |                      |
              +---------------------+----------------------+
                                    |
                                    v
                              Web API / WS


GStreamer RTSP Ingest
      |
      v
FrameQueue
      |
      v
TensorRT
      |
      +----> DetectionResult ----> AlertManager ----> SQLite + JPG
      |
      v
AnnotatedFrameQueue
      |
      v
GStreamer H.264
      |
      +----> Recorder
      |
      +----> EncodedAccessUnitQueue
      |             |
      |             v
      |        WebRtcManager
      |             |
      |        libdatachannel
      |             |
      |          skai-ice
      |             |
      |             v
      |          Browser
      |
      +----> optional RTSP
```

---

# Recommended Repository Layout

```text
.
├── CMakeLists.txt
├── README.md
├── ROADMAP.md
├── config/
│   └── config.example.yaml
├── include/skai/
│   ├── app/
│   ├── core/
│   ├── video/
│   ├── inference/
│   ├── gps/
│   ├── alerts/
│   ├── storage/
│   ├── recording/
│   ├── webrtc/
│   └── web/
├── src/
│   ├── main.cpp
│   ├── app/
│   ├── core/
│   ├── video/
│   ├── inference/
│   ├── gps/
│   ├── alerts/
│   ├── storage/
│   ├── recording/
│   ├── webrtc/
│   └── web/
├── tests/
│   ├── unit/
│   └── integration/
├── web/
│   ├── index.html
│   ├── app.js
│   └── style.css
├── models/
├── scripts/
├── systemd/
├── docs/
└── third_party/
    ├── skai-ice/          # pinned independent repository
    └── libdatachannel/    # pinned revision compatible with skai-ice
```

Do not copy `skai-ice` source files into this repository.

---

# API Draft

Initial API version:

```text
GET  /health

GET  /api/v1/status
GET  /api/v1/config
GET  /api/v1/detections/latest
GET  /api/v1/gps
GET  /api/v1/alerts
GET  /api/v1/alerts/{id}
GET  /api/v1/alerts?limit=100
GET  /api/v1/alerts?class=person
GET  /api/v1/alerts?from=<timestamp>&to=<timestamp>
GET  /api/v1/recordings

POST /api/v1/recording/start
POST /api/v1/recording/stop

POST /api/v1/detector/enable
POST /api/v1/detector/disable

POST   /api/v1/webrtc/whep
DELETE /api/v1/webrtc/sessions/{session_id}

GET  /ws

GET  /
GET  /app.js
GET  /style.css
```

`/ws` is for application status/events. It is not required for WebRTC signaling;
WHEP uses HTTP.

Avoid adding endpoints until there is a real caller.

---

# Milestones

## Milestone A — Headless perception

Complete through Step 11.

Result:

```text
RTSP URL
  ↓
GStreamer
  ↓
TensorRT YOLO
  ↓
annotated frames
```

No web UI required yet.

---

## Milestone B — Native edge server

Complete through Step 16.

Result:

```text
RTSP URL
  ↓
AI
  ↓
Boost.Beast
  ↓
browser status UI
```

At this point the project has fully replaced the need for ROS 2 + Flask/FastAPI for
its control plane.

---

## Milestone C — Edge appliance

Complete through Step 22.

Result:

```text
AI + GPS + SQLite-backed alerts + recording + browser control
```

This is the first version useful as a standalone field device.

---

## Milestone D — LAN browser video

Complete through Step 26.

Result:

```text
GStreamer H.264
      ↓
libdatachannel
      ↓
skai-ice
      ↓
Browser
```

No Agora, no MediaMTX requirement, no frontend framework, and no `webrtcbin`.

---

## Milestone E — Deployable product baseline

Complete through Step 36.

Result:

```text
systemd-managed native Jetson application
with health monitoring, testing, packaging, recovery,
SQLite persistence, and LAN WebRTC
```

---

# Development Order

Recommended implementation order:

```text
1. build/lifecycle + test framework
2. queue/runtime primitives
3. RTSP/GStreamer ingest
4. TensorRT
5. annotation
6. Boost.Beast HTTP
7. application WebSocket
8. vanilla web UI
9. GPS (fixed position: Taipei 101)
10. SQLite persistence
11. alerts
12. recording
13. skai-ice + libdatachannel integration
14. Beast WHEP
15. H.264 WebRTC media
16. WebRTC/ICE hardening
17. optional RTSP
18. observability
19. recovery
20. test coverage audit + end-to-end tests
21. packaging
22. optimization
23. security
```

Every step above is test-first (see 4.8): tests are written with, and before,
the implementation in the same step. Item 20 audits coverage; it is not where
testing starts.

The key rule is:

> Get one complete vertical path working before adding optional integrations.

Do not begin with WebRTC or advanced optimization.

The first meaningful end-to-end target should be:

```text
RTSP URL
  ↓
GStreamer
  ↓
TensorRT YOLO
  ↓
Boost.Beast status API
  ↓
plain browser UI
```

Then add GPS, SQLite alerts, recording, and finally the existing `skai-ice`
WebRTC path.

---

# Architectural Rules for Future Steps

1. No ROS 2 dependency.
2. No Python runtime dependency.
3. No Flask/FastAPI.
4. No Agora.
5. No JavaScript framework or npm build dependency.
6. RTSP URL is the only supported video input.
7. Do not add USB/V4L2, CSI/Argus, webcam, local-file, or generic source abstractions.
8. GStreamer owns RTSP ingest, decode, encode, and recording media pipelines.
9. Boost.Beast owns REST, WebSocket, static-file serving, and WHEP HTTP signaling.
10. TensorRT/CUDA owns inference.
11. SQLite persistence goes through repository/database boundaries; do not scatter `sqlite3_*` calls across modules.
12. Business logic must not depend on HTTP.
13. Media logic must not depend on the web UI.
14. libdatachannel owns PeerConnection, DTLS, SRTP, and RTP.
15. `skai-ice` is the only ICE implementation used by this application.
16. Do not copy `skai-ice` into this codebase; consume a pinned revision.
17. Do not use `cpp-httplib`, MediaMTX, or `webrtcbin` as a hidden second WebRTC path.
18. First WebRTC release is LAN/host-candidate only.
19. NAT traversal improvements belong in `skai-ice`.
20. Queues are bounded.
21. Slow consumers must not stall RTSP ingest or inference.
22. RTSP reconnect/stall recovery is a core product requirement.
23. Every long-running component must support clean shutdown.
24. Add abstractions only when at least two real implementations require them.
25. Profile before optimizing.
26. Prefer explicit, readable C++ over framework-like internal infrastructure.
27. Test-first: write failing tests for new behavior before implementing it; a step without tests for its new behavior is incomplete.
28. Test fixtures stay under `tests/` and never add a non-RTSP input path to the application.

---

# Final Target

The final system should conceptually be:

```text
                         Jetson Orin Nano
+------------------------------------------------------------------+
|                                                                  |
| RTSP URL                                                         |
|   ↓                                                              |
| GStreamer RTSP ingest                                            |
|   ↓                                                              |
| TensorRT / CUDA                                                  |
|   ↓                                                              |
| Detection / Alert                                                |
|   ├──────────────► SQLite metadata + JPG snapshot                |
|   ↓                                                              |
| GStreamer H.264                                                  |
|   ├──────────────► MP4 recording                                 |
|   ├──────────────► optional RTSP                                 |
|   ↓                                                              |
| libdatachannel                                                   |
|   ├─ PeerConnection                                              |
|   ├─ DTLS / SRTP / RTP                                           |
|   ↓                                                              |
| skai-ice                                                         |
|   └─ STUN / ICE                                                  |
|   ↓                                                              |
| Browser WebRTC                                                   |
|                                                                  |
| GPS ───────────────► Application State                           |
|                           ↓                                      |
|                  Boost.Asio / Beast                              |
|                    ├─ REST API                                   |
|                    ├─ WebSocket events                           |
|                    ├─ WHEP HTTP                                  |
|                    └─ static Vanilla JS UI                       |
|                                                                  |
+------------------------------------------------------------------+
```

No ROS 2.  
No Agora.  
No Flask/FastAPI.  
No frontend framework.  
No duplicate ICE stack.

The codebase should remain small enough that one engineer can trace a decoded RTSP frame
from capture through inference, alert persistence, H.264 encoding, recording, and
browser WebRTC delivery without crossing an opaque application framework boundary.

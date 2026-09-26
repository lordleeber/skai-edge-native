# Step 38-b: Orin Nano profiling baseline

The collector owns a timestamped RTSP fixture, the production `skai-edge`
executable and a Firefox session showing the actual Console UI. It observes live
source frames, the exact UI WHEP session, transmitted bytes, presentation callbacks
and inference progress throughout warmup and collection. It writes raw samples
and summaries to a fresh private output directory and shuts down its children.
The camera configuration supplied by the operator is read only; an isolated copy
selects the explicit test RTSP source, port 0, local storage/recording and disabled
WHIP. Recording and WHEP are enabled. Production still receives only RTSP.

## Run

From the checkout root, build the TensorRT-enabled Release executable:

```sh
cmake -S . -B build/profiling -DBUILD_TESTING=OFF -DSKAI_ENABLE_TENSORRT=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/profiling --target skai-edge -j2
```

The device collector requires `Release` and records compiler/CUDA flags alongside
binary, model, tool and UI hashes. The native runtime source revision and the
collector/UI fingerprints identify the experiment even when tools are measured
before their feature-branch commit.
 Profiling tools
require PyYAML, Python GI with Gst/GstRtspServer typelibs, Firefox, geckodriver,
OpenH264 and FFmpeg. On Ubuntu the additional Python/GStreamer packages are
`python3-yaml`, `python3-gi`, `gir1.2-gstreamer-1.0` and
`gir1.2-gst-rtsp-server-1.0`; the fixture also needs `x264enc`.
Use an interpreter that can import these modules. Python is a development-tool
dependency, not an installed native-runtime dependency.

Start a local driver in another terminal:

```sh
build/smoke-tools/geckodriver --host 127.0.0.1 --port 4444
```

Then select the existing engine and actual ICE interfaces through your local
config, and run:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 scripts/profile_jetson.py config/config.local.yaml \
  --binary build/profiling/skai-edge \
  --fixture-script tests/fixtures/profile_rtsp.py \
  --webdriver-url http://127.0.0.1:4444 \
  --openh264-dir /path/to/gmp-gmpopenh264/2.6.0 \
  --openh264-abi aarch64-gcc3 \
  --warmup 10 --seconds 60 --interval 1 \
  --output-dir build/profile-baseline-new-run
```

Stop the driver after profiling. The output directory must not already exist.
Exit 0 means a validated Orin Nano baseline, exit 1 failure. The explicit portable
harness flag can only produce exit 2 / `portable_harness_passed`, never a Jetson
baseline. A local Firefox PID and local WebDriver are required so the fixture
and browser share a host clock. This measures a receiver on the same Jetson;
external LAN camera/receiver behavior needs a separate experiment.

## Measurement definitions

See [production stage boundaries](step38-profiling-metrics.md). Successful-work
cumulative differences exclude warmup and give weighted stage means. Interval
means retain separate distributions: their p95 is a p95 of interval averages,
not a per-frame inference/queue p95. CPU/GPU/RSS/FPS percentiles also describe
samples. Raw counter endpoints, task identities, clocks and dropped frames remain
in `report.json`.

| Metric | Scope |
| --- | --- |
| Ingest FPS | Delivered RTSP frame-counter delta / monotonic observation time |
| RTSP ingest/decode CPU | Surviving named RTSP, decode/demux, NVIDIA/software decoder and V4L2 task tick deltas |
| Process CPU | Owned `skai-edge` process ticks; 100% is one CPU core |
| Preprocess / TensorRT / postprocess | Production stage counters, wall and CUDA timing separated |
| Annotation | Actual Console canvas draw submission wall time; server annotation has zero samples in this graph |
| Encode CPU | Not applicable: native graph passes source H.264 through; fixture x264 CPU belongs to another process |
| Encoded queue latency | Recording and exact UI WHEP queue entry-to-dequeue; excludes track-ready waits/network |
| WebRTC receiver media latency | Interval mean jitter-buffer residence + interval mean decode time |
| End-to-end browser latency | Source RGB timestamp to estimated browser presentation time |
| RAM | Owned process RSS, rather than all browser/OS memory |
| GPU utilization | Device-wide sysfs load snapshots, including cohosted browser work |

The receiver statistics follow the
[W3C definitions](https://www.w3.org/TR/webrtc-stats/). They do not include network
transit or compositor latency. Source timestamps are encoded as a checked
96-bit marker (magic, run nonce, Unix milliseconds, CRC-8 with polynomial 0x07 and initial value 0). A canvas reads 3 KiB
from every fifth presented frame; every sampled marker must validate. The source
and browser monotonic-origin/wall-clock difference is checked. End-to-end latency
uses [expectedDisplayTime](https://wicg.github.io/video-rvfc/), so it estimates
presentation rather than measuring screen photons. The observer's own wall cost
is recorded separately; it remains part of this instrumented workload.

Task attribution is scoped: decoder/conversion workers are included by their
names, while generic/shared threads remain in the raw inventory and total CPU.
New/exited tasks are reported, and surviving-task totals cannot account for CPU
consumed by a task that exits between samples. GPU snapshots are not a continuous
or process-specific GPU utilization trace. No clock/power settings are changed;
power mode, CPU governor and observed frequencies are recorded for comparison.

The synthetic fixture is 1280×720 at 30 FPS, with a moving box and timestamp band.
This establishes a repeatable synthetic workload, including the real engine,
hardware decode, Console overlay code, recording and browser receiver. A camera
scene with many detections, remote network delay or an external browser needs its
own baseline. Fixture/browser CPU contention and the timestamp observer are
included in the experiment, while the process CPU/RSS statistics belong to
`skai-edge` alone.

## Recorded baseline

The local result and exact environment are recorded in
[the Orin Nano baseline](baselines/orin-nano-2026-09-26.md).
The compact JSON retains summary statistics and artifact fingerprints; the full
local `report.json` retains all samples. These are local/operator results, not
independent CI evidence.

## Regression checks

```sh
ctest --test-dir build -L profiling --output-on-failure
ctest --test-dir build --output-on-failure
```

GoogleTest checks cumulative delta math, warmup exclusion, invalid values,
receiver/session/counter resets, CPU task identity, frozen media, clock drift,
raw percentiles, compiler flags/Release policy, precision-preserving long-running
timing JSON and child cleanup. A protocol failure test starts two owned
portable children, checks failure evidence and preserved source config, and
verifies both PIDs exited. Node checks the timestamp format, checksum, nonce,
ambiguous pixels and the actual UI peer/canvas timing hook. Hardware observations
come from the explicit local run above, rather than portable test doubles.

# Step 35: Jetson smoke tests

Run from the repository root after building with TensorRT enabled. The script
starts the real `skai-edge` executable with the configured RTSP stream and engine.
It copies YAML into a new private directory under ignored `build/`, allocates an
HTTP port through the server's own bind, and uses fresh SQLite, alert and recording
paths. It enables recording and LAN WHEP and disables WHIP in this isolated run.
Three peer slots allow the console, SDP probe and companion browser to coexist.
Your original configuration, database and running cloud publisher are preserved.

## Dependencies and local run

Besides the production runtime, install `python3-yaml`, Firefox, geckodriver,
`ffmpeg` and `ffprobe`. No Selenium Python package is required: the probe uses
the [WebDriver HTTP protocol](https://firefox-source-docs.mozilla.org/testing/geckodriver/Usage.html).
New Firefox profiles need OpenH264 for WHEP. For a local driver, the script can
copy only the installed codec from `~/.mozilla/firefox/*/gmp-gmpopenh264/*` into
a disposable [encoded test profile](https://firefox-source-docs.mozilla.org/testing/geckodriver/Profiles.html).
It does not copy cookies, history or the rest of your profile. Alternatively,
pass `--openh264-dir /path/to/gmp-gmpopenh264/VERSION` and
`--openh264-abi aarch64-gcc3` (use the browser machine's ABI for a remote driver).

```sh
cmake -S . -B build -DSKAI_ENABLE_TENSORRT=ON
cmake --build build -j2
scripts/smoke_test_jetson.sh config/config.local.yaml \
  --host 172.16.1.50 --geckodriver /path/to/geckodriver \
  --alert-class chair --seconds 120
```

Use your Jetson's LAN IPv4 address and a class actually visible in the RTSP
scene. Before launching the process, the runner checks that this address is
assigned to a local interface and is not loopback. The example `chair` is specific to the camera used during development.
`--alert-class` creates a one-frame rule at confidence 0.1 in the isolated YAML;
the detector's configured confidence threshold still applies. Omitting it
preserves your configured rules. Missing detections or alerts cause a timeout,
not fabricated inference results or direct database inserts.

## Checks and evidence

| Check | Evidence |
| --- | --- |
| Jetson | `aarch64` and `/etc/nv_tegra_release` |
| Engine and inference | Nonempty configured engine file; production detector reports positive FPS and non-null inference latency |
| RTSP | Source metrics report connected with decoded frames |
| API and health | Real HTTP status, metrics and RUNNING readiness |
| Browser UI and video | Firefox loads the production console; nonzero video dimensions, advancing time and increasing frame callbacks/playback counters |
| WHEP SDP | Browser creates an offer, checks HTTP 201/SDP/Location, accepts the answer and deletes the session with HTTP 204 |
| ICE interfaces | Connected media peer has sent bytes and diagnostics identify an interface in the configured whitelist |
| Observation window | Throughout `--seconds`, polls every 0.5 seconds require advancing browser time/frames, RTSP decoded frames and bytes sent by the same connected ICE peer |
| Alert persistence | A new API alert and snapshot exist; after shutdown, read-only SQLite reopen/integrity check finds the same alert, snapshot and detection classes |
| Recording | HTTP stop finalizes recording, every MP4 is decoded by FFmpeg to EOS, and ffprobe counts positive video frames |
| Shutdown | The owned process exits zero after SIGTERM and logs `skai-edge stopped`; forced termination fails |

Firefox's WebRTC playback counter can stay zero in headless mode; the probe also
counts [video frame callbacks](https://developer.mozilla.org/en-US/docs/Web/API/HTMLVideoElement/requestVideoFrameCallback).
It requires both new frames and advancing media time. A fetched HTML page or a
successful SDP response alone cannot pass the browser/media check.
The observation lasts at least the requested duration and one polling interval;
the report retains elapsed time, sample count and starting/ending counters.

Logs, isolated configuration, SQLite, snapshots, MP4s and `report.json` remain in
the reported directory for inspection. The directory is mode 0700 and copied
YAML is mode 0600. SIGINT/SIGTERM and failure paths clean up only owned processes.
Existing output directories are rejected to avoid overwriting evidence.

## LAN browser acceptance

The local headless browser uses the Jetson's LAN URL, but this does not by itself
prove reception on another device. A local run returns **2 / incomplete** after
all automated checks pass, until the LAN acceptance is recorded.

During the `--seconds 120` window, open the printed **Browser URL** from another
LAN device and observe continuously playing video. After the run completes,
record that observation for its exact URL:

```sh
scripts/smoke_test_jetson.sh \
  --confirm-lan-report build/smoke-jetson-RUN/report.json \
  --observed-url 'http://172.16.1.50:ACTUAL_PORT/#smoke=RUN_ID'
```

This is an explicit operator attestation. It requires successful automated
Jetson checks and refuses a different URL or run ID. Copy the entire printed URL,
including its fragment: each run generates a random 128-bit ID, so a reused port
cannot accept an earlier run's observation. Reports without a run ID are rejected.
It cannot turn a failed run or
a `--skip-device-check` portable run into successful Jetson acceptance.

For automatic acceptance from another LAN computer, use its Firefox WebDriver
endpoint with `--webdriver-url http://COMPANION_LAN_IP:4444`. That driver must
launch Firefox on the companion device, with compatible OpenH264. The script
requires a literal private IPv4 endpoint outside the Jetson's local interface
addresses before classifying it as remote. A local driver advertised with the
Jetson's LAN IP still leaves the report incomplete.

Exit codes: **0** full acceptance, **1** failed check, **2** automated checks passed
but LAN/device acceptance incomplete. `--skip-device-check` exists for the
portable script harness and always keeps acceptance incomplete.

## Regression tests and development evidence

`SmokeScript.*` uses GoogleTest and a portable fake executable/WebDriver/media
tool protocol fixture. These tests check script control flow, failure handling,
process cleanup, source-YAML preservation and manual confirmation policy. They
do not claim GPU execution or real browser coverage. Run:

```sh
ctest --test-dir build -R '^SmokeScript\.' --output-on-failure
ctest --test-dir build --output-on-failure
```

On 2026-09-26, the real Jetson/L4T R36.4.7 run used the configured RTSP camera,
the existing TensorRT engine, Firefox 147 and geckodriver 0.37.1. The automated
run received 1280×720 live video, accepted/deleted the WHEP session, selected
`wlP1p1s0`, persisted a real `chair` alert, decoded the finalized MP4 to EOS and
shut down cleanly. Its local-browser report correctly remained incomplete.

The operator separately confirmed continuously playing video from another LAN
device at `http://172.16.1.50:41173/`, served by an isolated instance of the same
build. This is separate manual LAN evidence; it was not attached to the different
port in the automated report. The existing cloud publisher continued running.

## Review follow-ups

Step 35-a pins `SKAI_SMOKE_PYTHON` to CMake's checked `Python3_EXECUTABLE`
for every `SmokeScript.*` test. A regression shadows PATH's `python3` with a
failing interpreter and requires the runner to use the configured interpreter.

Step 35-b validates the assigned LAN address before starting the owned process,
monitors browser/RTSP/peer progress throughout the observation window and adds
the random run ID to both the report and browser URL. Regressions freeze media
after the initial browser check, disconnect the peer, freeze its byte counter or
RTSP frames, reuse a port across run IDs and attempt legacy confirmation.
Both parts are required for final Step 35 acceptance; the split keeps each
implementation/test diff below 800 lines.
The development results remain local/operator evidence, not independent CI.

The reviewed runner was rerun on the real Jetson with a 10-second window. It
collected 20 samples over 10.50 seconds: Firefox decoded frames increased from
6 to 216, RTSP frames from 201 to 463, and the same `wlP1p1s0` peer's sent bytes
from 177,736 to 2,398,422. TensorRT, WHEP, SQLite reopen, MP4 decode and clean
shutdown also passed. The identified URL and counters are retained in
`build/smoke-review35-b/report.json`. This new local-browser run remains incomplete;
the earlier LAN observation is not reused for its new run ID.

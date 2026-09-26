# Step 34 integration tests

The tests compose the production Application lifecycle, RTSP source, GPS,
AlertManager, SQLite repository, recorder, Beast HTTP server, WebRtcManager,
libdatachannel and skai-ice. They let the HTTP server bind ephemeral loopback ports directly with `web.port: 0` and keep YAML,
SQLite, snapshots and recordings in a unique temporary directory removed on
teardown. WHIP is disabled in these deterministic tests.

| Test | Behavior checked |
| --- | --- |
| `Pipeline.PersistsRtspDetectionsAlertsSnapshotsAndPlayableRecording` | A fixed white patch is published as H.264 through the RTSP fixture, decoded, detected, persisted with GPS and a JPEG snapshot, exposed through real HTTP routes, and recorded. Recording stops through HTTP; SQLite is reopened after shutdown and each finalized MP4 is decoded to frames and EOS. |
| `Pipeline.ProductionInferenceInvalidatesFailuresAndPublishesRecovery` | Injected backend failures invalidate API detections and emit the production unavailable event; recovery emits detections and persists alerts through the production async queue. |
| `Pipeline.WhepRouteDeliversRtspH264AndDeletesConnectedPeer` | A receiver gathers an offer through skai-ice, sends it to the Beast WHEP route, applies the answer, connects and receives multiple H.264 frames from the RTSP source. HTTP status remains accessible; DELETE removes the peer and a repeated DELETE returns 404. |
| `PipelineJetson.PersistsRtspDetectionsAlertsSnapshotsAndPlayableRecording` | Runs the same persistence and recording chain with the real YoloInferenceModule and TensorRT engine, using the generated gradient image from the existing inference regression tests. |

The portable inference backend is compiled only into the test executable. It
locates white pixels and returns a known bounding box with COCO class `person`.
Both variants use the real YoloInferenceModule for frame consumption, API
commits, event publication, failure invalidation, annotation and bounded async
alert persistence. The backend interface contains only model loading and
inference; it cannot replace that orchestration. A backend factory keeps these
portable tests available when TensorRT and CUDA are disabled.

The Jetson variant uses
`/var/lib/skai-edge/models/yolo11s_fp16.engine` and expects the existing engine's
`umbrella` regression result at confidence 0.1; a different engine may require a
new reference image and expectation. That synthetic result is a model regression
check, not evidence of detection accuracy on real umbrellas.

The PNG file is read by the test RTSP server, encoded as constrained-baseline
H.264 and delivered over RTSP. The application still has exactly one RTSP input;
no file-input path or test backend is added to the production executable.
The fixture refuses image changes while running and validates the file and PNG
plugins before publishing.

## Commands

```sh
cmake -S . -B build
cmake --build build -j2
ctest --test-dir build -R '^Pipeline\.' --output-on-failure
ctest --test-dir build -LE jetson --output-on-failure
ctest --test-dir build --output-on-failure
```

The portable tests have the `rtsp-webrtc` label, matching both `ctest -L rtsp`
and `ctest -L webrtc`. The GPU variant is built only when the inference module
is available and has the `rtsp-jetson` label, so `ctest -LE jetson` excludes it.
The tests currently compose production lifecycle adapters rather than launching
`main`; executable launch and signal handling remain covered by `Process.*`.

## Cloud uplink check on 2026-09-26

The live server at `https://skai-cam.duckdns.org/watch` returned HTTP 200. With
RTSP restored to `rtsp://100.69.117.102:8554/test`, an isolated local configuration
and the existing WHIP_TOKEN, the real `skai-edge` executable ran for 30 seconds:

- WHIP session creation succeeded; ICE reached `completed` and the peer was
  `connected` to the server's public candidate.
- The publisher sent 733 H.264 access units, including 487 SEI units carrying
  727 attached inference results; results dropped and boxes dropped were zero.
- Detector FPS samples averaged 24.56, with median inference time 29.4 ms.
- The service deleted the WHIP session and shut down cleanly.

The alternate RTSP address `100.88.83.93` did not provide a decoded frame during
an earlier check. Its WHIP signaling and ICE succeeded, but no video was sent.
The restored address was used for the successful media run.

These observations confirm HTTPS signaling, ICE connectivity and outgoing
media; they do not confirm a browser received or rendered it. Browser acceptance
remains a manual check: open `/watch` from a second device, start the configured
publisher, confirm moving video and detection overlay, then stop the publisher
and verify the viewer reports the disconnect. Cloud availability and credentials
are deliberately not prerequisites for the default CTest suite.

## Review fixes, part A

The deterministic double now replaces only InferenceBackend. A failure/recovery
regression test observes the real module's API invalidation and detection
events, and the persistence test exercises its async alert worker. YAML accepts
port 0 while rejecting negative/out-of-range ports; the server owns the port
allocation from bind onward, removing the release-before-bind reservation.
The separate runtime-composition review finding is addressed in part B.

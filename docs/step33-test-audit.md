# Step 33 test audit

The table maps each ROADMAP audit item to a behavior test. The SQLite audit
also added `Database.RejectsSchemaFromNewerApplicationVersion`, which was the
missing migration rejection case.

| Area | Existing or added tests |
| --- | --- |
| Configuration | `Config.ParsesValidSettings`, `Config.RejectsIncorrectTypes`, `Config.RejectsUnknownKeys` |
| Bounded queue | `BoundedQueue.ConcurrentProducersAndConsumersPreserveValues`, `BoundedQueue.ShutdownWakesBlockedConsumer` |
| Routing | `HttpRouter.RejectsUnknownRoutesAndUnsupportedMethods`, `HttpRouter.ServesHealthAndRuntimeStatusJson` |
| Static root confinement and traversal | `HttpServer.ConfinesStaticRequestsToConfiguredRoot` (plain and encoded traversal, symlink escape) |
| JSON serialization | `DetectionSei.SerializesResultsAndEmptyResults`, `HttpRouter.ExposesRuntimeMetricsAndUnavailableValues`, `EventChannel.PublishesEverySupportedEventTypeToMultipleSubscribers` |
| WHEP route validation | `WhepHttpApi.CreatesAndDeletesSessionWithoutWebSocket` (SDP, content type, method) |
| WebRTC session registry | `WebRtcManager.CreatesUniqueAnswersEnforcesCapacityAndClosesSessions`, `WebRtcManager.ReapsStaleSessions` |
| Alert rules | `AlertManager.RequiresClassConfidenceAndConsecutiveFrames`, `AlertManager.FiltersByNormalizedRoiUsingBoxCenter`, `AlertManager.EnforcesCooldownAndPublishesGpsAlertEvent` |
| SQLite migrations | `Database.CreatesParentDirectoryAndAppliesMigrationIdempotently`, `Database.RejectsSchemaFromNewerApplicationVersion` |
| AlertRepository CRUD and queries | `AlertRepository.InsertsAndReadsOneAlertWithDetections`, `AlertRepository.CountsStoredAlertsAndReportsUnavailableDatabase`, `AlertRepository.FiltersAndRemovesOldestAlerts` |
| Transaction rollback | `AlertRepository.RollsBackAlertWhenDetectionInsertionFails` |
| Fixed GPS and config validation | `GpsSource.ReturnsConfiguredCoordinatesUnchanged`, `Config.RejectsInvalidGpsCoordinate`, `Config.RejectsInvalidGpsLongitudeAndSource` |
| YOLO postprocessing | `YoloPostprocess.FiltersScoresSuppressesSameClassAndRestoresCoordinates`, `YoloPostprocess.RejectsInvalidInputsAndKeepsResultsDeterministic` |
| State model | `HealthWatchdog.ReportsEveryComponentAndAggregatesFailures`, `HealthModel.FailedWhipUplinkDegradesHealthyLanWebrtc` |

## Portable and instrumented runs

All presets disable TensorRT so the tests can run on ordinary x86 Linux with
the dependencies in `AGENTS.md`. The same presets can be checked on Jetson.

```sh
cmake --preset x86
cmake --build --preset x86
ctest --test-dir build -LE jetson --output-on-failure

cmake --preset x86-asan-ubsan
cmake --build --preset x86-asan-ubsan
ctest --preset x86-asan-ubsan

cmake --preset x86-tsan
cmake --build --preset x86-tsan
ctest --preset x86-tsan
```

The ASan/UBSan test preset runs the full x86-safe suite. The TSan test preset
selects queue, lifecycle, event/WebSocket, and WebRTC session tests. Both
exclude `jetson`. The repository tests the WebRTC integration boundary and
does not repeat `skai-ice`'s STUN/ICE protocol tests.

The TSan preset requires Clang and compiler-rt. It covers LAN WHEP and cloud
WHIP publisher, peer-state, and metrics tests. The full `WhipSei` case remains
in the normal and ASan suites because its GStreamer/GLib source fixture reports
races from libraries that are not TSan-instrumented.

The suppression in `config/tsan.supp` matches only vendored
`Channel::resetCallbacks` during Track shutdown. The observed report's top
frame is `__interceptor_memcpy`, so `race_top` cannot target that vendor method.
Any race involving this exact teardown method can still be hidden; review the
rule after upgrading `libdatachannel`. Project code remains instrumented.

The ASan/UBSan preset disables LeakSanitizer because its per-process exit scan
exceeds existing test timeouts on Jetson, while address and undefined-behavior
checks remain active.

Run `scripts/coverage.sh` with `gcovr` 8.6 or newer on `PATH` for text, HTML,
and Cobertura reports under `build/x86-coverage/report/`. The gate requires at
least 90% line coverage in each of `cli.cpp`, `preprocess.cpp`,
`tensor_layout.cpp`, and `yolo_postprocess.cpp`; it fails if any file has no
coverage data. The script removes previous `.gcda` counts before CTest, so
repeated runs measure only the current suite.

# Step 31 failure recovery

`recoverable` means the request or frame fails while the affected worker keeps
running. `restart-module` means the affected media pipeline is torn down and
rebuilt without restarting the process. `fatal-process` means startup cannot
provide a required service, so initialization fails and the application exits
with an error. A failed optional recording request is never fatal to capture or
streaming.

| Failure | Class | Handling and verification |
| --- | --- | --- |
| RTSP endpoint unavailable | restart-module | Bounded retry and reconnect; `RtspRecovery.ConnectsWhenEndpointAppearsAfterOfflineStartup`. |
| RTSP disconnect or server restart | restart-module | Tear down and reopen the source; `RtspRecovery.RecoversAfterServerRestartWithoutChangingUrl`, `RtspSource.ReconnectClearsPreviousFpsBeforeMeasuringChangedStream`. |
| RTSP stall | restart-module | Frame timeout triggers pipeline rebuild; `RtspRecovery.DetectsStallAndReconnectsWhenFramesResume`. |
| RTP loss or jitter degradation | recoverable, then restart-module on sustained frame stall | Jitter buffer reports packet loss; source health degrades and reconnects if frames stop; `RtspRecovery.DetectsPacketLossAndFrameStallWithoutSocketError`. |
| Malformed decoded frame or empty H.264 access unit | recoverable | Ignore the invalid sample; a continuing stream supplies the next frame. A sustained lack of valid frames triggers RTSP stall recovery. An empty access unit does not consume the next unit's discontinuity marker. |
| TensorRT engine missing or incompatible at startup | fatal-process | Detector initialization fails; `TensorRtEngine.ReportsMissingEmptyAndIncompatibleFiles`. |
| TensorRT failure on one frame | recoverable | Clear detector health, log the error, forward the original frame to the optional annotated output, and try the next frame; `AnnotationDetector.FailedInferencePassesFrameThroughAndProcessesNextFrame`. |
| Encoder invalid frame | recoverable | Reject the frame, report the error, process the next frame; `H264Encoder.RejectsFramesOutsideConstrainedBaselineLevel31`. |
| Encoder pipeline or appsrc failure | restart-module | Close the failed pipeline and rebuild for the next valid frame; `H264Encoder.RebuildsForDimensionChangesAndEmitsNewKeyframe` covers pipeline rebuild. |
| Recording disk full or storage error | recoverable | Stop recording and keep `state=error` with `last_error` until an explicit retry or stop. RTSP and WebRTC continue; `RecordingModule.DiskSpaceFailureStaysVisibleWhileWorkerContinues`. |
| GPS timeout | deferred | The current fixed GPS source has no timeout; test when real GPS hardware support is added. |
| Browser disconnect | recoverable | Close and reap only that WebRTC or WebSocket session; `WebRtcManager.SupportsTwoConnectedViewersAndRepeatedSessionCleanup`, `HttpServer.ShutdownClosesConnectedWebSocketPromptly`. |
| Slow WebSocket client | recoverable | Bound both the event inbox before dispatch and the socket's outgoing queue; drop oldest pending events, keeping latest status. `EventInbox.BoundsPendingEventsAndSchedulesOnlyOneDrain`. |
| Malformed WHEP offer | recoverable | Reject the request and remove its session; `WebRtcManager.CreatesUniqueAnswersEnforcesCapacityAndClosesSessions`. |
| libdatachannel PeerConnection creation or media failure | recoverable | Return a signaling error without leaking a session, or close and reap the failed peer; `WebRtcManager.PeerConstructionFailureDoesNotEscapeOrLeakSession`. |
| skai-ice gathering or connectivity failure | recoverable | Return gathering timeout or mark the peer failed, then reap it; `WebRtcManager.ReapsStaleSessions` verifies connection timeout cleanup and `WebRtcDependency.ConfiguresSkaiIceBeforePeerConnectionGathering` verifies the dependency configuration. |
| WebRTC media queue backpressure | recoverable | Drop queued access units and wait for a keyframe for that peer; `WebRtcManager.WaitsForKeyframeAfterPerPeerQueueDropsAccessUnits`. |

The scope of each recovery is the affected frame, recording session, source
pipeline, or browser peer. Initialization of required configuration and
dependencies remains a startup error.

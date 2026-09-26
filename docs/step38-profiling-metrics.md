# Step 38-a: production profiling metrics

`GET /api/v1/metrics` now includes `profiling`, with cumulative count, total,
mean, minimum and maximum milliseconds for each stage. Counts start again when
the runtime initializes. These summaries describe completed work; they do not
prove stream freshness. Zero samples produce null mean/min/max, while a measured
zero remains zero. Negative and non-finite measurements are rejected.

| Stage | Measurement boundary |
| --- | --- |
| `preprocess_wall` | Existing CUDA preprocessor wall time, including upload and synchronization |
| `preprocess_gpu` | Existing CUDA event measurement |
| `inference_wall` | TensorRT enqueue through output download and stream synchronization |
| `inference_gpu` | CUDA events around TensorRT enqueue |
| `postprocess_wall` | CPU YOLO decode/filter/NMS |
| `annotation_wall` | Server `annotate_frame` when an annotated output is requested |
| `recording_queue_wait` | Encoded recording queue entry to dequeue, including dequeued units later skipped |

Inference stages are published only for successfully committed detections. The
production graph sends original H.264 to recording/WHEP/WHIP and paints detection
boxes in the browser. It does not run the server annotation or H.264 encoder;
those stages must be reported as not applicable for this graph, rather than
invented zero-cost measurements.

Each WHEP peer also exposes `media_queue_wait`, measured from its own queue entry
to dequeue. A cached keyframe gets a new entry time. Waiting for track readiness,
RTP packetization, network travel, browser decoding and presentation are outside
this queue measurement. RTP PTS is never subtracted from a steady-clock value.

RTSP capture and decode/demux queue workers have stable `skai-rtsp`, `skai-decode`
and `skai-demux` names for external `/proc` CPU attribution. Hardware decoder work
and shared library workers must be accounted for separately; these names do not
promise complete per-component CPU accounting.

GPU sampling supports the Orin `gpu.0/load` sysfs paths as well as the older
`17000000.gpu/load` path. Readings outside 0–1000 permille remain unavailable.
Step 38-b will record a timed Orin Nano baseline with browser/source evidence.

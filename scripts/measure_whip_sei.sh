#!/usr/bin/env bash
# Step 28 on-camera check: run skai-edge for a fixed time, sample detector FPS
# from the status API, then stop it and print the WHIP SEI session summary.
# If a recording directory is given, also confirm no MP4 carries the SEI UUID.
#
# Usage: scripts/measure_whip_sei.sh CONFIG SECONDS [PORT] [RECORDING_DIR]
# Run from the directory whose .env holds WHIP_TOKEN (normally the repo root).
set -euo pipefail

config=${1:?config path}
seconds=${2:?duration in seconds}
port=${3:-8080}
recordings=${4:-}
binary=${SKAI_EDGE_BINARY:-build/skai-edge}
log=$(mktemp)
samples=$(mktemp)

setsid "$binary" --config "$config" >"$log" 2>&1 &
pid=$!

stop_edge() {
    kill -INT -- "-$pid" 2>/dev/null || true
    for _ in $(seq 1 30); do
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.5
    done
    kill -KILL -- "-$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}
trap 'stop_edge; rm -f "$samples"' EXIT

deadline=$((SECONDS + seconds))
while ((SECONDS < deadline)); do
    kill -0 "$pid" 2>/dev/null || { echo "skai-edge exited early"; cat "$log"; exit 1; }
    curl -s --max-time 1 "http://127.0.0.1:$port/api/v1/status" >>"$samples" || true
    echo >>"$samples"
    sleep 2
done
stop_edge
trap 'rm -f "$samples"' EXIT

python3 - "$samples" <<'EOF'
import json, statistics, sys
fps, ms = [], []
for line in open(sys.argv[1]):
    try:
        detector = json.loads(line)["detector"]
    except (ValueError, KeyError):
        continue
    if detector.get("fps") is not None: fps.append(detector["fps"])
    if detector.get("last_inference_ms") is not None: ms.append(detector["last_inference_ms"])
steady = fps[len(fps) // 4:]  # skip warm-up samples
if steady:
    print(f"detector fps: samples={len(steady)} mean={statistics.mean(steady):.2f} "
          f"min={min(steady):.2f} max={max(steady):.2f}")
if ms:
    print(f"last_inference_ms: median={statistics.median(ms):.1f} max={max(ms):.1f}")
EOF
grep -E 'module=whip' "$log" | sed -E 's/^timestamp="[^"]*" //' || echo "no WHIP log lines"
if [[ -n "$recordings" ]]; then
    uuid=$'\x19\x7c\x65\xee\xa1\x32\x4f\x53\x85\xa0\x8a\xf9\xad\x15\x3c\x4b'
    files=$(find "$recordings" -name '*.mp4' | wc -l)
    hits=$({ find "$recordings" -name '*.mp4' -exec env LC_ALL=C grep -laF "$uuid" {} + ||
             true; } | wc -l)
    echo "recordings: mp4 files=$files files containing the SEI UUID=$hits"
fi
echo "log: $log"
if pgrep -f "$binary --config $config" >/dev/null; then
    echo "skai-edge is still running" >&2
    exit 1
fi

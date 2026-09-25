(() => {
  "use strict";

  const byId = (id) => document.getElementById(id);
  const setText = (id, value) => { byId(id).textContent = String(value); };
  const count = (value) => value == null ? "—" : Number(value).toLocaleString();
  const decimal = (value, unit = "") => value == null || !Number.isFinite(Number(value))
    ? "—" : `${Number(value).toFixed(1)}${unit}`;
  const title = (value) => value ? String(value).replace(/_/g, " ")
    .replace(/\b\w/g, (letter) => letter.toUpperCase()) : "Unavailable";

  async function read(path) {
    const controller = new AbortController();
    const timeout = window.setTimeout(() => controller.abort(), 5000);
    try {
      const response = await fetch(path, {cache: "no-store", signal: controller.signal});
      if (!response.ok && !(response.status === 503 &&
          (path === "/api/v1/recordings" || path === "/api/v1/gps"))) {
        throw new Error(`Telemetry request failed: ${response.status}`);
      }
      return await response.json();
    } finally {
      window.clearTimeout(timeout);
    }
  }

  function renderStatus(status, metrics) {
    const detector = status?.detector;
    const fps = status?.video?.fps ?? metrics?.ingest_fps;
    setText("source-fps", fps == null ? "No measured frames" : `${decimal(fps)} fps ingested`);
    setText("inference-fps", detector?.enabled === false ? "Disabled" :
      decimal(detector?.fps ?? metrics?.inference_fps, " fps"));
    setText("inference-latency", detector?.enabled === false ? "Detector disabled" :
      `Inference ${decimal(detector?.last_inference_ms ?? metrics?.inference_latency_ms, " ms")}`);
  }

  function renderSource(source) {
    setText("source-health", title(source?.health));
    setText("source-transport", source?.transport?.toUpperCase() || "—");
    setText("source-format", source?.codec ? `${source.codec} · ${source.width || "—"} × ${source.height || "—"}` : "—");
    setText("source-last-frame", source?.last_frame_age_ms >= 0
      ? `${count(source.last_frame_age_ms)} ms ago` : "—");
    setText("source-frames", count(source?.frames_received));
    setText("source-reconnects", count(source?.reconnect_count));
    setText("source-queue-loss", `${count(source?.frames_dropped)} / ${count(source?.frames_discarded)}`);
    setText("source-packet-loss", `${count(source?.packets_lost)} / ${count(source?.packets_late)}`);
    setText("source-error", source?.last_error || "No source error reported");
  }

  function renderPipeline(metrics) {
    const inference = metrics?.inference_queue;
    const encoded = metrics?.encoded_media_queue;
    setText("encoder-mode", title(metrics?.encoder_mode));
    setText("inference-depth", count(inference?.depth));
    setText("inference-loss", `${count(inference?.dropped)} / ${count(inference?.discarded)}`);
    setText("encoded-depth", count(encoded?.depth));
    setText("encoded-loss", `${count(encoded?.dropped)} / ${count(encoded?.discarded)}`);
    setText("encoder-fps", decimal(metrics?.encoder_fps, " fps"));
  }

  function renderPeers(webrtc) {
    const rows = byId("peers");
    rows.replaceChildren();
    const peers = webrtc?.peers || [];
    setText("peer-count", webrtc ? count(webrtc.active_peers ?? peers.length) : "—");
    if (!peers.length) {
      const cell = rows.insertRow().insertCell();
      cell.colSpan = 4;
      cell.className = "empty";
      cell.textContent = webrtc ? "No active LAN peers" : "Peer data unavailable";
      return;
    }
    for (const peer of peers) {
      const row = rows.insertRow();
      row.insertCell().textContent = peer.session_id || "—";
      row.insertCell().textContent = `${title(peer.peer_state)} / ${title(peer.ice_state)}`;
      row.insertCell().textContent = peer.selected_interface || peer.local_interface || "—";
      row.insertCell().textContent = peer.local_candidate || "—";
    }
  }

  function renderDelivery(metrics) {
    const whip = metrics?.whip;
    setText("whip-state", title(whip?.peer_state));
    setText("whip-ice", title(whip?.ice_state));
    setText("whip-sent", count(whip?.access_units_sent));
    setText("whip-loss", `${count(whip?.media_queue_drops)} / ${count(whip?.media_queue_discarded)}`);
    setText("whip-error", whip?.last_error || "No uplink error reported");
    setText("websocket-count", count(metrics?.websocket_clients));
    renderPeers(metrics?.webrtc);
  }

  function renderRecording(recording) {
    setText("recording-state", recording?.available ? title(recording.state) : "Unavailable");
    setText("recording-units", count(recording?.access_units_written));
    setText("recording-error", recording?.last_error || recording?.unavailable_reason ||
      "No recording error reported");
  }

  function renderGps(gps, metrics) {
    const available = gps?.available && gps.valid !== false;
    setText("gps-source", available ? gps.source === "fixed" ? "Fixed / simulated" :
      title(gps.source) : "Unavailable");
    setText("gps-position", available && Number.isFinite(gps.latitude) &&
      Number.isFinite(gps.longitude) ?
      `${gps.latitude.toFixed(6)}, ${gps.longitude.toFixed(6)}` : "—");
    setText("gps-age", metrics?.gps?.age_s == null ?
      gps?.source === "fixed" ? "Not applicable to fixed source" : "—" :
      decimal(metrics.gps.age_s, " s"));
  }

  function renderErrors(errors) {
    const list = byId("errors");
    list.replaceChildren();
    if (!errors?.length) {
      const item = document.createElement("li");
      item.className = "empty";
      item.textContent = "No recent errors";
      list.append(item);
      return;
    }
    for (const error of [...errors].reverse()) {
      const item = document.createElement("li");
      item.textContent = `${error.data?.module || "system"} · ${error.data?.message || "Unknown error"}`;
      item.title = error.timestamp || "";
      list.append(item);
    }
  }

  let refreshing = false;
  async function refresh() {
    if (refreshing) return;
    refreshing = true;
    try {
      const paths = ["/api/v1/status", "/api/v1/metrics",
        "/api/v1/recordings", "/api/v1/gps"];
      const results = await Promise.allSettled(paths.map(read));
      const values = results.map((result) => result.status === "fulfilled" ? result.value : null);
      const [status, metrics, recording, gps] = values;
      renderStatus(status, metrics);
      renderSource(metrics?.rtsp);
      renderPipeline(metrics);
      renderDelivery(metrics);
      renderRecording(recording);
      renderGps(gps, metrics);
      renderErrors(metrics?.recent_errors);
      setText("fetch-state", results.every((result) => result.status === "fulfilled") ?
        "Live telemetry" : "Some telemetry is unavailable");
      setText("updated-at", `Updated ${new Date().toLocaleTimeString()}`);
    } finally {
      refreshing = false;
    }
  }

  refresh();
  window.setInterval(refresh, 3000);
})();

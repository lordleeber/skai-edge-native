(() => {
  "use strict";

  const byId = (id) => document.getElementById(id);
  const setText = (id, value) => { byId(id).textContent = value; };
  const number = (value, suffix = "") => value == null ? `—${suffix}` : `${Number(value).toFixed(1)}${suffix}`;
  const initialRetryDelayMs = 1000;
  const maximumRetryDelayMs = 30000;
  const connectionAttemptTimeoutMs = 8000;
  const revisions = {status: 0, gps: 0, detection: 0};
  let retryDelayMs = initialRetryDelayMs;
  let retryTimer = 0;
  let activeSocket = null;
  let latestDetectionSequence = -1;
  let bootstrapPromise;
  const alertIds = new Set();

  async function getJson(path) {
    const controller = new AbortController();
    const timeout = window.setTimeout(() => controller.abort(), 5000);
    try {
      const response = await fetch(path, {
        headers: {Accept: "application/json"},
        signal: controller.signal
      });
      if (!response.ok) throw new Error(`${response.status} ${response.statusText}`);
      return await response.json();
    } finally {
      window.clearTimeout(timeout);
    }
  }

  function renderStatus(data) {
    const videoFps = data.video?.fps ?? data.video_fps;
    const detectorFps = data.detector?.fps ?? data.detector_fps;
    const inferenceMs = data.detector?.last_inference_ms ?? data.last_inference_ms;
    const detectorEnabled = data.detector?.enabled ?? data.detector_enabled;
    setText("system-status", data.status || "Unknown");
    setText("uptime", `Uptime ${Math.floor(data.uptime_s || 0)}s`);
    setText("video-fps", number(videoFps, " fps"));
    setText("video-state", videoFps == null ? "Waiting for frames" : "Receiving frames");
    setText("detector-state", detectorEnabled == null ? "Unavailable" :
      detectorEnabled ? "Enabled" : "Disabled");
    setText("detector-fps", number(detectorFps, " fps"));
    setText("inference-time", `Inference ${number(inferenceMs, " ms")}`);
    byId("system-status").dataset.state = data.status;
    stamp();
  }

  function renderGps(data) {
    const fixed = data.available && data.valid !== false && data.source === "fixed";
    setText("gps-state", fixed ? "Fixed / simulated" : "Unavailable");
    setText("gps-position", data.available ? `${data.latitude.toFixed(6)}, ${data.longitude.toFixed(6)}` : "Coordinates —");
  }

  function renderDetections(data) {
    const detections = data.detections || [];
    const available = data.available ?? Number.isFinite(data.frame_sequence);
    const sequence = Number(data.frame_sequence);
    if (available && Number.isFinite(sequence) && sequence < latestDetectionSequence) return;
    latestDetectionSequence = available && Number.isFinite(sequence) ? sequence : -1;
    const rows = byId("detections");
    rows.replaceChildren();
    setText("detection-count", `${detections.length} object${detections.length === 1 ? "" : "s"}`);
    if (!detections.length) {
      const row = rows.insertRow();
      const cell = row.insertCell();
      cell.colSpan = 3;
      cell.className = "empty";
      cell.textContent = available ? "No objects detected" : "No detections yet";
      return;
    }
    detections.slice(0, 10).forEach((item) => {
      const row = rows.insertRow();
      row.insertCell().textContent = item.class_name || `Class ${item.class_id}`;
      row.insertCell().textContent = `${(item.confidence * 100).toFixed(1)}%`;
      row.insertCell().textContent = data.frame_sequence;
    });
  }

  function addAlert(data, kind = "Alert") {
    if (data.id && alertIds.has(data.id)) return;
    if (data.id) alertIds.add(data.id);
    const list = byId("alerts");
    if (list.firstElementChild?.classList.contains("empty")) list.replaceChildren();
    const item = document.createElement("li");
    const title = document.createElement("strong");
    title.textContent = kind;
    const detail = document.createElement("span");
    detail.textContent = data.message || data.class ||
      data.detections?.[0]?.class_name || "New event";
    item.append(title, detail);
    list.prepend(item);
    while (list.children.length > 6) list.lastElementChild.remove();
  }

  function mergeAlerts(data) {
    [...(data.items || [])].reverse().forEach((alert) => addAlert(alert));
  }

  function stamp() {
    setText("last-update", `Updated ${new Date().toLocaleTimeString()}`);
  }

  function setConnection(label, state) {
    setText("connection", label);
    const dot = byId("connection-dot");
    dot.classList.toggle("online", state === "online");
    dot.classList.toggle("offline", state === "offline");
  }

  function handleEvent(event) {
    let message;
    try { message = JSON.parse(event.data); } catch { return; }
    if (message.type === "status") {
      ++revisions.status;
      renderStatus(message.data);
    }
    if (message.type === "gps") {
      ++revisions.gps;
      renderGps(message.data);
    }
    if (message.type === "detection") {
      ++revisions.detection;
      renderDetections(message.data);
    }
    if (message.type === "alert") addAlert(message.data);
    if (message.type === "system_error") addAlert(message.data, "System error");
    if (message.type === "recording") setText("recording-state", message.data.active ? "Recording" : "Not recording");
  }

  function scheduleReconnect() {
    setConnection("Disconnected", "offline");
    window.clearTimeout(retryTimer);
    const delay = retryDelayMs;
    retryDelayMs = Math.min(retryDelayMs * 2, maximumRetryDelayMs);
    retryTimer = window.setTimeout(() => {
      setConnection("Reconnecting", "connecting");
      connect();
    }, delay);
  }

  function connect() {
    if (activeSocket && activeSocket.readyState < WebSocket.CLOSING) return;
    setConnection("Connecting", "connecting");
    const scheme = location.protocol === "https:" ? "wss" : "ws";
    let socket;
    try {
      socket = new WebSocket(`${scheme}://${location.host}/ws`);
      activeSocket = socket;
    } catch {
      scheduleReconnect();
      return;
    }
    const attemptTimer = window.setTimeout(() => {
      if (activeSocket !== socket || socket.readyState !== WebSocket.CONNECTING) return;
      activeSocket = null;
      socket.close();
      scheduleReconnect();
    }, connectionAttemptTimeoutMs);
    socket.addEventListener("open", async () => {
      window.clearTimeout(attemptTimer);
      if (activeSocket !== socket) return socket.close();
      setConnection("Synchronizing", "connecting");
      await bootstrapPromise;
      await initialLoad();
      if (activeSocket !== socket || socket.readyState !== WebSocket.OPEN) return;
      retryDelayMs = initialRetryDelayMs;
      setConnection("Live", "online");
    });
    socket.addEventListener("message", (event) => {
      if (activeSocket === socket) handleEvent(event);
    });
    socket.addEventListener("error", () => {
      if (activeSocket === socket) setConnection("Disconnected", "offline");
    });
    socket.addEventListener("close", () => {
      window.clearTimeout(attemptTimer);
      if (activeSocket !== socket) return;
      activeSocket = null;
      latestDetectionSequence = -1;
      scheduleReconnect();
    });
  }

  async function loadSnapshot(key, path, render, unavailable) {
    const revision = revisions[key];
    let data;
    try {
      data = await getJson(path);
    } catch {
      data = unavailable;
    }
    if (revision === revisions[key]) render(data);
  }

  async function initialLoad() {
    await Promise.all([
      loadSnapshot("status", "/api/v1/status", renderStatus,
        {status: "unavailable", uptime_s: 0}),
      loadSnapshot("gps", "/api/v1/gps", renderGps, {available: false}),
      loadSnapshot("detection", "/api/v1/detections/latest", renderDetections,
        {available: false, detections: []}),
      getJson("/api/v1/alerts?limit=6").then(mergeAlerts).catch(() => {})
    ]);
  }

  async function recording(action) {
    try {
      const response = await fetch(`/api/v1/recording/${action}`, {method: "POST"});
      const data = await response.json();
      if (!response.ok) throw new Error(data.error || "Recording unavailable");
    } catch (error) {
      addAlert({message: error.message}, "Recording");
    }
  }

  byId("record-start").addEventListener("click", () => recording("start"));
  byId("record-stop").addEventListener("click", () => recording("stop"));
  bootstrapPromise = initialLoad();
  connect();
})();

(() => {
  "use strict";

  const byId = (id) => document.getElementById(id);
  const setText = (id, value) => { byId(id).textContent = value; };
  const number = (value, suffix = "") => value == null ? `—${suffix}` : `${Number(value).toFixed(1)}${suffix}`;

  async function getJson(path) {
    const response = await fetch(path, {headers: {Accept: "application/json"}});
    if (!response.ok) throw new Error(`${response.status} ${response.statusText}`);
    return response.json();
  }

  function renderStatus(data) {
    const videoFps = data.video?.fps ?? data.video_fps;
    const detectorFps = data.detector?.fps ?? data.detector_fps;
    const inferenceMs = data.detector?.last_inference_ms ?? data.last_inference_ms;
    setText("system-status", data.status || "Unknown");
    setText("uptime", `Uptime ${Math.floor(data.uptime_s || 0)}s`);
    setText("video-fps", number(videoFps, " fps"));
    setText("video-state", videoFps == null ? "Waiting for frames" : "Receiving frames");
    setText("detector-fps", number(detectorFps, " fps"));
    setText("inference-time", `Inference ${number(inferenceMs, " ms")}`);
    byId("system-status").dataset.state = data.status;
    stamp();
  }

  function renderGps(data) {
    setText("gps-state", data.available ? "Position fixed" : "Unavailable");
    setText("gps-position", data.available ? `${data.latitude.toFixed(6)}, ${data.longitude.toFixed(6)}` : "Coordinates —");
  }

  function renderDetections(data) {
    const rows = byId("detections");
    rows.replaceChildren();
    const detections = data.detections || [];
    const available = data.available ?? Number.isFinite(data.frame_sequence);
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
    const list = byId("alerts");
    if (list.firstElementChild?.classList.contains("empty")) list.replaceChildren();
    const item = document.createElement("li");
    const title = document.createElement("strong");
    title.textContent = kind;
    const detail = document.createElement("span");
    detail.textContent = data.message || data.class || "New event";
    item.append(title, detail);
    list.prepend(item);
    while (list.children.length > 6) list.lastElementChild.remove();
  }

  function stamp() {
    setText("last-update", `Updated ${new Date().toLocaleTimeString()}`);
  }

  function handleEvent(event) {
    let message;
    try { message = JSON.parse(event.data); } catch { return; }
    if (message.type === "status") renderStatus(message.data);
    if (message.type === "gps") renderGps(message.data);
    if (message.type === "detection") renderDetections(message.data);
    if (message.type === "alert") addAlert(message.data);
    if (message.type === "system_error") addAlert(message.data, "System error");
    if (message.type === "recording") setText("recording-state", message.data.active ? "Recording" : "Not recording");
  }

  function connect() {
    const scheme = location.protocol === "https:" ? "wss" : "ws";
    const socket = new WebSocket(`${scheme}://${location.host}/ws`);
    socket.addEventListener("open", () => {
      setText("connection", "Live");
      byId("connection-dot").classList.add("online");
    });
    socket.addEventListener("message", handleEvent);
    socket.addEventListener("close", () => {
      setText("connection", "Reconnecting");
      byId("connection-dot").classList.remove("online");
      window.setTimeout(connect, 2000);
    });
  }

  async function initialLoad() {
    const tasks = [
      getJson("/api/v1/status").then(renderStatus),
      getJson("/api/v1/gps").then(renderGps),
      getJson("/api/v1/detections/latest").then(renderDetections)
    ];
    await Promise.allSettled(tasks);
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
  initialLoad();
  connect();
})();

(() => {
  "use strict";

  const byId = (id) => document.getElementById(id);
  const setText = (id, value) => { byId(id).textContent = value; };
  const number = (value, suffix = "") => value == null ? `—${suffix}` : `${Number(value).toFixed(1)}${suffix}`;
  const initialRetryDelayMs = 1000;
  const maximumRetryDelayMs = 30000;
  const connectionAttemptTimeoutMs = 8000;
  const revisions = {status: 0, gps: 0, detection: 0, recording: 0};
  let retryDelayMs = initialRetryDelayMs;
  let retryTimer = 0;
  let activeSocket = null;
  let latestDetectionSequence = -1;
  let bootstrapPromise;
  let activePeer = null;
  let activeWhepLocation = "";
  let videoRetryTimer = 0;
  let videoGeneration = 0;
  let latestOverlay = null;
  let detectionFrames = [];
  let overlayEnabled = true;
  const alertIds = new Set();

  function drawDetectionOverlay() {
    const canvas = byId("video-overlay");
    const video = byId("live-video");
    const width = canvas.clientWidth || video.clientWidth || 0;
    const height = canvas.clientHeight || video.clientHeight || 0;
    const ratio = window.devicePixelRatio || 1;
    if (canvas.width !== Math.round(width * ratio)) canvas.width = Math.round(width * ratio);
    if (canvas.height !== Math.round(height * ratio)) canvas.height = Math.round(height * ratio);
    const context = canvas.getContext("2d");
    context.setTransform(ratio, 0, 0, ratio, 0, 0);
    context.clearRect(0, 0, width, height);
    if (!overlayEnabled || !latestOverlay || width <= 0 || height <= 0) return;
    const sourceWidth = Number(latestOverlay.frame_width) || video.videoWidth;
    const sourceHeight = Number(latestOverlay.frame_height) || video.videoHeight;
    if (!(sourceWidth > 0 && sourceHeight > 0)) return;
    const scale = Math.min(width / sourceWidth, height / sourceHeight);
    const offsetX = (width - sourceWidth * scale) / 2;
    const offsetY = (height - sourceHeight * scale) / 2;
    context.lineWidth = 2;
    context.font = "600 12px ui-monospace, monospace";
    context.textBaseline = "top";
    for (const detection of latestOverlay.detections || []) {
      if (!Array.isArray(detection.box) || detection.box.length !== 4) continue;
      const [x1, y1, x2, y2] = detection.box.map(Number);
      if (![x1, y1, x2, y2].every(Number.isFinite)) continue;
      const x = offsetX + x1 * scale;
      const y = offsetY + y1 * scale;
      const boxWidth = Math.max(0, (x2 - x1) * scale);
      const boxHeight = Math.max(0, (y2 - y1) * scale);
      const label = `${detection.class_name || `Class ${detection.class_id}`} ${(Number(detection.confidence) * 100).toFixed(1)}%`;
      context.strokeStyle = "#63e6be";
      context.fillStyle = "rgba(7, 18, 15, .82)";
      context.strokeRect(x, y, boxWidth, boxHeight);
      const labelWidth = context.measureText(label).width + 10;
      const labelY = Math.max(0, y - 20);
      context.fillRect(x, labelY, labelWidth, 20);
      context.fillStyle = "#eef4f5";
      context.fillText(label, x + 5, labelY + 3);
    }
  }

  function ptsToRtpTimestamp(ptsNs) {
    const value = Number(ptsNs);
    if (!Number.isFinite(value) || value < 0) return null;
    const seconds = Math.floor(value / 1e9);
    const remainder = value - seconds * 1e9;
    return (seconds * 90000 + Math.floor(remainder * 90000 / 1e9)) >>> 0;
  }

  function signedRtpDistance(left, right) {
    const difference = (left - right) >>> 0;
    return difference > 0x7fffffff ? difference - 0x100000000 : difference;
  }

  function presentDetectionFrame(rtpTimestamp) {
    const timestamp = Number(rtpTimestamp) >>> 0;
    let selected = null;
    let selectedDistance = Number.POSITIVE_INFINITY;
    for (const frame of detectionFrames) {
      const distance = Math.abs(signedRtpDistance(frame.rtpTimestamp, timestamp));
      if (distance <= 9000 && distance < selectedDistance) {
        selected = frame.data;
        selectedDistance = distance;
      }
    }
    latestOverlay = selected;
    detectionFrames = detectionFrames.filter((frame) =>
      signedRtpDistance(frame.rtpTimestamp, timestamp) >= -9000);
    drawDetectionOverlay();
  }

  function scheduleVideoFrameOverlay(generation) {
    const video = byId("live-video");
    if (generation !== videoGeneration || !video.requestVideoFrameCallback) return;
    video.requestVideoFrameCallback((_now, metadata) => {
      if (generation !== videoGeneration) return;
      if (Number.isFinite(metadata?.rtpTimestamp)) {
        presentDetectionFrame(metadata.rtpTimestamp);
      }
      scheduleVideoFrameOverlay(generation);
    });
  }

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
    if (detectorEnabled === false) {
      detectionFrames = [];
      latestOverlay = null;
      drawDetectionOverlay();
    }
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
    const rtpTimestamp = available ? ptsToRtpTimestamp(data.pts_ns) : null;
    if (available && rtpTimestamp != null && byId("live-video").requestVideoFrameCallback) {
      detectionFrames.push({rtpTimestamp, data});
      if (detectionFrames.length > 60) detectionFrames.shift();
    } else {
      latestOverlay = available ? data : null;
      drawDetectionOverlay();
    }
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

  function renderRecording(data) {
    setText("recording-state", data?.active ? "Recording" :
      data?.state === "error" ? "Recording error" : "Not recording");
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
    if (message.type === "recording") { ++revisions.recording; renderRecording(message.data); }
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
      loadSnapshot("recording", "/api/v1/recordings", renderRecording,
        {active: false, state: "stopped"}),
      getJson("/api/v1/alerts?limit=6").then(mergeAlerts).catch(() => {})
    ]);
  }

  async function recording(action) {
    try {
      const response = await fetch(`/api/v1/recording/${action}`, {method: "POST"});
      const data = await response.json();
      if (!response.ok) throw new Error(data.error || "Recording unavailable");
      renderRecording(data);
    } catch (error) {
      addAlert({message: error.message}, "Recording");
    }
  }

  function waitForIceGathering(pc) {
    if (pc.iceGatheringState === "complete") return Promise.resolve();
    return new Promise((resolve, reject) => {
      const timeout = window.setTimeout(() => {
        pc.removeEventListener("icegatheringstatechange", changed);
        reject(new Error("ICE gathering timed out"));
      }, connectionAttemptTimeoutMs);
      function changed() {
        if (pc.iceGatheringState !== "complete") return;
        window.clearTimeout(timeout);
        pc.removeEventListener("icegatheringstatechange", changed);
        resolve();
      }
      pc.addEventListener("icegatheringstatechange", changed);
    });
  }

  function closeVideoSession(removeRemote = true) {
    ++videoGeneration;
    window.clearTimeout(videoRetryTimer);
    const location = activeWhepLocation;
    activeWhepLocation = "";
    if (activePeer) activePeer.close();
    activePeer = null;
    byId("live-video").srcObject = null;
    detectionFrames = [];
    latestOverlay = null;
    drawDetectionOverlay();
    byId("video-placeholder").classList.toggle("hidden", false);
    if (removeRemote && location) {
      fetch(location, {method: "DELETE", keepalive: true}).catch(() => {});
    }
  }

  function retryVideo(generation) {
    if (generation !== videoGeneration) return;
    setText("live-stream-state", "Reconnecting");
    videoRetryTimer = window.setTimeout(connectVideo, 2000);
  }

  async function connectVideo() {
    closeVideoSession();
    const generation = videoGeneration;
    setText("live-stream-state", "Connecting");
    const pc = new RTCPeerConnection();
    activePeer = pc;
    const transceiver = pc.addTransceiver("video", {direction: "recvonly"});
    if (transceiver?.receiver && "playoutDelayHint" in transceiver.receiver) {
      transceiver.receiver.playoutDelayHint = 0.25;
    }
    pc.addEventListener("track", (event) => {
      if (generation !== videoGeneration) return;
      byId("live-video").srcObject = event.streams[0] || new MediaStream([event.track]);
      byId("video-placeholder").classList.toggle("hidden", true);
      setText("live-stream-state", "Live");
      drawDetectionOverlay();
      scheduleVideoFrameOverlay(generation);
    });
    pc.addEventListener("connectionstatechange", () => {
      if (generation !== videoGeneration) return;
      if (["failed", "disconnected", "closed"].includes(pc.connectionState)) {
        closeVideoSession();
        retryVideo(videoGeneration);
      }
    });
    try {
      await pc.setLocalDescription(await pc.createOffer());
      await waitForIceGathering(pc);
      const response = await fetch("/api/v1/webrtc/whep", {
        method: "POST",
        headers: {"Content-Type": "application/sdp"},
        body: pc.localDescription.sdp
      });
      if (!response.ok) throw new Error(`WHEP ${response.status}`);
      const location = response.headers.get("Location");
      const answer = await response.text();
      if (generation !== videoGeneration) {
        if (location) fetch(location, {method: "DELETE", keepalive: true}).catch(() => {});
        return;
      }
      activeWhepLocation = location || "";
      await pc.setRemoteDescription({type: "answer", sdp: answer});
    } catch {
      if (generation !== videoGeneration) return;
      closeVideoSession();
      retryVideo(videoGeneration);
    }
  }

  async function initializeVideo() {
    let config;
    try {
      config = await getJson("/api/v1/config");
      overlayEnabled = config.detector?.annotate !== false;
      drawDetectionOverlay();
    } catch {
      setText("live-stream-state", "Unavailable");
      return;
    }
    if (typeof RTCPeerConnection === "undefined") {
      setText("live-stream-state", "Unsupported");
      return;
    }
    if (config.webrtc?.enabled === false) {
      setText("live-stream-state", "Disabled");
      return;
    }
    connectVideo();
    window.addEventListener("pagehide", () => closeVideoSession());
  }

  byId("record-start").addEventListener("click", () => recording("start"));
  byId("record-stop").addEventListener("click", () => recording("stop"));
  window.addEventListener("resize", drawDetectionOverlay);
  bootstrapPromise = Promise.all([initialLoad(), initializeVideo()]);
  connect();
})();

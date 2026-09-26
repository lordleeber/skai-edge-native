const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const appSource = fs.readFileSync(
  path.join(__dirname, "..", "..", "web", "app.js"), "utf8");

class Element {
  constructor() {
    this.textContent = "";
    this.dataset = {};
    this.children = [];
    this.parent = null;
    this.clientWidth = 640;
    this.clientHeight = 360;
    this.videoWidth = 1280;
    this.videoHeight = 720;
    this.drawCalls = [];
    this.videoFrameCallback = null;
    const classes = new Set();
    this.classList = {
      contains: (name) => classes.has(name),
      toggle: (name, enabled) => enabled ? classes.add(name) : classes.delete(name)
    };
  }

  get firstElementChild() { return this.children[0] || null; }
  get lastElementChild() { return this.children.at(-1) || null; }
  addEventListener() {}
  replaceChildren(...children) { this.children = []; this.append(...children); }
  append(...children) {
    children.forEach((child) => { child.parent = this; this.children.push(child); });
  }
  prepend(child) { child.parent = this; this.children.unshift(child); }
  remove() {
    if (this.parent) this.parent.children = this.parent.children.filter((item) => item !== this);
  }
  insertRow() { const row = new Element(); this.append(row); return row; }
  insertCell() { const cell = new Element(); this.append(cell); return cell; }
  getContext() {
    return {
      clearRect: (...args) => this.drawCalls.push(["clearRect", ...args]),
      strokeRect: (...args) => this.drawCalls.push(["strokeRect", ...args]),
      fillRect: (...args) => this.drawCalls.push(["fillRect", ...args]),
      fillText: (...args) => this.drawCalls.push(["fillText", ...args]),
      measureText: (value) => ({width: String(value).length * 7}),
      setTransform: (...args) => this.drawCalls.push(["setTransform", ...args])
    };
  }
  requestVideoFrameCallback(callback) { this.videoFrameCallback = callback; }
  presentVideoFrame(rtpTimestamp) {
    const callback = this.videoFrameCallback;
    this.videoFrameCallback = null;
    if (callback) callback(0, {rtpTimestamp});
  }
}

function createHarness({withRtc = false, webrtcEnabled = true,
  overlayEnabled = true} = {}) {
  const elements = new Map();
  const element = (id) => {
    if (!elements.has(id)) {
      const created = new Element();
      if (id === "live-video" && !withRtc) {
        created.requestVideoFrameCallback = undefined;
      }
      elements.set(id, created);
    }
    return elements.get(id);
  };
  const timers = new Map();
  let timerId = 0;
  const timerApi = {
    setTimeout(callback, delay) {
      const id = ++timerId;
      timers.set(id, {callback, delay});
      return id;
    },
    clearTimeout(id) { timers.delete(id); }
  };

  const snapshots = {
    status: {status: "running", uptime_s: 10, video: {fps: 30},
      detector: {enabled: false, fps: null, last_inference_ms: null}},
    gps: {available: true, valid: true, source: "fixed",
      latitude: 25.033964, longitude: 121.564468},
    detections: {available: true, frame_sequence: 5, detections: [
      {class_id: 0, class_name: "person", confidence: 0.9}
    ]},
    alerts: {available: true, items: [{id: "a-1", detections: [
      {class_name: "person"}]}]},
    config: {webrtc: {enabled: webrtcEnabled}, detector: {annotate: overlayEnabled}}
  };
  const fetchCalls = [];
  const fetchRequests = [];
  const fetch = async (url, options = {}) => {
    fetchCalls.push(url);
    fetchRequests.push({url, options});
    if (url === "/api/v1/webrtc/whep") {
      return {ok: true, status: 201, headers: {get: () => "/api/v1/webrtc/sessions/test"},
        text: async () => "answer-sdp"};
    }
    if (options.method === "DELETE") return {ok: true, status: 204};
    const value = url.endsWith("/status") ? snapshots.status
      : url.endsWith("/config") ? snapshots.config
      : url.endsWith("/gps") ? snapshots.gps
      : url.includes("/alerts?") ? snapshots.alerts : snapshots.detections;
    return {ok: true, status: 200, statusText: "OK",
      json: async () => structuredClone(value)};
  };

  class MockWebSocket {
    static CONNECTING = 0;
    static OPEN = 1;
    static CLOSING = 2;
    static CLOSED = 3;
    static instances = [];

    constructor(url) {
      this.url = url;
      this.readyState = MockWebSocket.CONNECTING;
      this.listeners = new Map();
      this.closeCalls = 0;
      MockWebSocket.instances.push(this);
    }
    addEventListener(type, callback) {
      const listeners = this.listeners.get(type) || [];
      listeners.push(callback);
      this.listeners.set(type, listeners);
    }
    emit(type, event = {}) {
      for (const listener of this.listeners.get(type) || []) listener(event);
    }
    open() { this.readyState = MockWebSocket.OPEN; this.emit("open"); }
    message(message) { this.emit("message", {data: JSON.stringify(message)}); }
    close() {
      ++this.closeCalls;
      if (this.readyState === MockWebSocket.CLOSED) return;
      this.readyState = MockWebSocket.CLOSED;
      this.emit("close");
    }
  }

  class MockPeerConnection {
    static instances = [];
    constructor() {
      this.iceGatheringState = "complete";
      this.connectionState = "new";
      this.listeners = new Map();
      MockPeerConnection.instances.push(this);
    }
    addTransceiver() { return {receiver: {playoutDelayHint: 0}}; }
    addEventListener(type, callback) { this.listeners.set(type, callback); }
    removeEventListener() {}
    async getStats() { return new Map([['inbound', {type: 'inbound-rtp', kind: 'video', id: 'inbound'}]]); }
    async createOffer() { return {type: "offer", sdp: "offer-sdp"}; }
    async setLocalDescription(description) { this.localDescription = description; }
    async setRemoteDescription(description) { this.remoteDescription = description; }
    close() { this.connectionState = "closed"; }
    emit(type, event = {}) { this.listeners.get(type)?.(event); }
  }

  const context = {
    AbortController,
    console,
    document: {getElementById: element, createElement: () => new Element()},
    fetch,
    location: {protocol: "http:", host: "edge.test"},
    structuredClone,
    WebSocket: MockWebSocket,
    window: {...timerApi, addEventListener() {}, devicePixelRatio: 1,
      performance: {now: (() => { let tick = 0; return () => tick += .25; })()}}
  };
  if (withRtc) context.RTCPeerConnection = MockPeerConnection;
  vm.runInNewContext(appSource, context, {filename: "app.js"});

  return {
    elements,
    profile: context.window.skaiProfileSnapshot,
    fetchCalls,
    fetchRequests,
    peers: MockPeerConnection.instances,
    snapshots,
    sockets: MockWebSocket.instances,
    runTimer(delay) {
      const found = [...timers].find(([, timer]) => timer.delay === delay);
      assert.ok(found, `missing ${delay}ms timer`);
      timers.delete(found[0]);
      found[1].callback();
    },
    hasTimer(delay) { return [...timers.values()].some((timer) => timer.delay === delay); }
  };
}

async function flush() {
  for (let index = 0; index < 30; ++index) await Promise.resolve();
}

function detectionFrame(harness) {
  const rows = harness.elements.get("detections");
  return rows.children[0].children[2]?.textContent;
}

test("REST bootstrap is resynchronized after the socket opens", async () => {
  const harness = createHarness();
  await flush();
  assert.equal(harness.sockets.length, 1);
  harness.sockets[0].open();
  await flush();

  assert.equal(harness.elements.get("connection").textContent, "Live");
  assert.equal(harness.elements.get("detector-state").textContent, "Disabled");
  assert.equal(harness.elements.get("gps-state").textContent, "Fixed / simulated");
  assert.equal(harness.elements.get("alerts").children.length, 1);
  assert.equal(harness.fetchCalls.filter((path) => path.endsWith("/status")).length, 2);

  harness.sockets[0].message({type: "status", data: {
    status: "running", uptime_s: 11, video_fps: 30,
    detector_enabled: true, detector_fps: 18, last_inference_ms: 40
  }});
  assert.equal(harness.elements.get("detector-state").textContent, "Enabled");
});

test("newer socket detections win and reconnect clears stale detections", async () => {
  const harness = createHarness();
  await flush();
  const first = harness.sockets[0];
  first.open();
  first.message({type: "detection", data: {available: true, frame_sequence: 6,
    detections: [{class_id: 2, class_name: "car", confidence: 0.8}]}});
  await flush();
  assert.equal(detectionFrame(harness), 6);

  first.close();
  harness.snapshots.detections = {available: false, frame_sequence: 0, detections: []};
  harness.snapshots.alerts.items.unshift({id: "a-2", detections: [{class_name: "car"}]});
  harness.runTimer(1000);
  assert.equal(harness.sockets.length, 2);
  harness.sockets[1].open();
  await flush();

  assert.equal(harness.elements.get("detections").children[0].children[0].textContent,
    "No detections yet");
  assert.equal(harness.elements.get("alerts").children.length, 2);
});

test("detection metadata draws bounding boxes over the unmodified video", async () => {
  const harness = createHarness();
  await flush();
  const socket = harness.sockets[0];
  socket.open();
  socket.message({type: "detection", data: {available: true, frame_sequence: 6,
    frame_width: 1280, frame_height: 720,
    detections: [{class_id: 2, class_name: "car", confidence: 0.8,
      box: [128, 72, 640, 360]}]}});

  const calls = harness.elements.get("video-overlay").drawCalls;
  assert.ok(calls.some(([name, x, y, width, height]) =>
    name === "strokeRect" && x === 64 && y === 36 && width === 256 && height === 144));
  assert.ok(calls.some(([name, label]) => name === "fillText" && label === "car 80.0%"));
});

test("detection overlay is selected by source PTS when its video frame is presented", async () => {
  const harness = createHarness({withRtc: true});
  await flush();
  const peer = harness.peers[0];
  peer.emit("track", {streams: [{}], track: {}});
  const socket = harness.sockets[0];
  socket.open();
  socket.message({type: "detection", data: {available: true, frame_sequence: 6,
    pts_ns: 2_000_000_000, frame_width: 1280, frame_height: 720,
    detections: [{class_name: "car", confidence: 0.8, box: [128, 72, 640, 360]}]}});

  const overlay = harness.elements.get("video-overlay");
  assert.equal(overlay.drawCalls.some(([name]) => name === "strokeRect"), false);
  harness.elements.get("live-video").presentVideoFrame(180000);
  assert.equal(overlay.drawCalls.some(([name]) => name === "strokeRect"), true);
});

test("unavailable detection event clears the table and pending overlay", async () => {
  const harness = createHarness({withRtc: true});
  await flush();
  harness.peers[0].emit("track", {streams: [{}], track: {}});
  const socket = harness.sockets[0];
  socket.open();
  socket.message({type: "detection", data: {available: true, frame_sequence: 6,
    pts_ns: 2_000_000_000, frame_width: 1280, frame_height: 720,
    detections: [{class_name: "car", confidence: 0.8, box: [128, 72, 640, 360]}]}});
  socket.message({type: "detection", data: {available: false}});

  assert.equal(harness.elements.get("detections").children[0].children[0].textContent,
    "No detections yet");
  const overlay = harness.elements.get("video-overlay");
  const strokesBefore = overlay.drawCalls.filter(([name]) => name === "strokeRect").length;
  harness.elements.get("live-video").presentVideoFrame(180000);
  assert.equal(overlay.drawCalls.filter(([name]) => name === "strokeRect").length,
    strokesBefore);
});

test("disabling the detector clears the last browser overlay", async () => {
  const harness = createHarness();
  await flush();
  const socket = harness.sockets[0];
  socket.open();
  socket.message({type: "detection", data: {available: true, frame_sequence: 6,
    frame_width: 1280, frame_height: 720,
    detections: [{class_name: "car", confidence: 0.8, box: [128, 72, 640, 360]}]}});
  const overlay = harness.elements.get("video-overlay");
  const clearsBefore = overlay.drawCalls.filter(([name]) => name === "clearRect").length;

  socket.message({type: "status", data: {status: "running",
    detector: {enabled: false}}});
  const clearsAfter = overlay.drawCalls.filter(([name]) => name === "clearRect").length;
  assert.ok(clearsAfter > clearsBefore);
  const strokesBefore = overlay.drawCalls.filter(([name]) => name === "strokeRect").length;
  harness.elements.get("live-video").presentVideoFrame(180000);
  assert.equal(overlay.drawCalls.filter(([name]) => name === "strokeRect").length,
    strokesBefore);
});

test("disabled annotation config leaves the source video unobscured", async () => {
  const harness = createHarness({overlayEnabled: false});
  await flush();
  const socket = harness.sockets[0];
  socket.open();
  socket.message({type: "detection", data: {available: true, frame_sequence: 6,
    frame_width: 1280, frame_height: 720,
    detections: [{class_name: "car", confidence: 0.8, box: [128, 72, 640, 360]}]}});

  assert.equal(harness.elements.get("video-overlay").drawCalls.some(
    ([name]) => name === "strokeRect"), false);
});

test("a stuck handshake is closed and advances exponential retry", async () => {
  const harness = createHarness();
  await flush();
  harness.runTimer(8000);
  assert.equal(harness.sockets[0].closeCalls, 1);
  assert.equal(harness.elements.get("connection").textContent, "Disconnected");
  assert.ok(harness.hasTimer(1000));

  harness.runTimer(1000);
  assert.equal(harness.sockets.length, 2);
  harness.runTimer(8000);
  assert.ok(harness.hasTimer(2000));
});

test("browser negotiates the WHEP video track with a completed local SDP", async () => {
  const harness = createHarness({withRtc: true});
  await flush();

  const request = harness.fetchRequests.find(({url}) => url === "/api/v1/webrtc/whep");
  assert.ok(request);
  assert.equal(request.options.method, "POST");
  assert.equal(request.options.headers["Content-Type"], "application/sdp");
  assert.equal(request.options.body, "offer-sdp");
  assert.equal(harness.peers[0].remoteDescription.sdp, "answer-sdp");
});

test("disabled WebRTC does not create or retry WHEP sessions", async () => {
  const harness = createHarness({withRtc: true, webrtcEnabled: false});
  await flush();

  assert.equal(harness.fetchCalls.includes("/api/v1/webrtc/whep"), false);
  assert.equal(harness.elements.get("live-stream-state").textContent, "Disabled");
  assert.equal(harness.hasTimer(2000), false);
});


test("profiling samples the actual UI peer and copied canvas timing totals", async () => {
  const harness = createHarness({withRtc: true});
  await flush();
  harness.peers[0].connectionState = "connected";
  const first = await harness.profile();
  assert.equal(first.state, "connected");
  assert.equal(first.peer, "test");
  assert.equal(first.stats[0].id, "inbound");
  assert.ok(first.annotation.count > 0);
  assert.ok(first.annotation.total_ms > 0);
  first.annotation.total_ms = -1;
  assert.ok((await harness.profile()).annotation.total_ms > 0);
});

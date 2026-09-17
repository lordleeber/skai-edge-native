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
}

function createHarness() {
  const elements = new Map();
  const element = (id) => {
    if (!elements.has(id)) elements.set(id, new Element());
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
      {class_name: "person"}]}]}
  };
  const fetchCalls = [];
  const fetch = async (url) => {
    fetchCalls.push(url);
    const value = url.endsWith("/status") ? snapshots.status
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

  const context = {
    AbortController,
    console,
    document: {getElementById: element, createElement: () => new Element()},
    fetch,
    location: {protocol: "http:", host: "edge.test"},
    structuredClone,
    WebSocket: MockWebSocket,
    window: timerApi
  };
  vm.runInNewContext(appSource, context, {filename: "app.js"});

  return {
    elements,
    fetchCalls,
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
  for (let index = 0; index < 12; ++index) await Promise.resolve();
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

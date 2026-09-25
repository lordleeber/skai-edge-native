const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const source = fs.readFileSync(path.join(__dirname, "../../web/diagnostics.js"), "utf8");

class Element {
  constructor() { this.textContent = ""; this.children = []; }
  replaceChildren(...children) { this.children = children; }
  append(...children) { this.children.push(...children); }
  insertRow() { const row = new Element(); this.children.push(row); return row; }
  insertCell() { const cell = new Element(); this.children.push(cell); return cell; }
  set innerHTML(_) { throw new Error("untrusted data must not use innerHTML"); }
}

async function render(responses) {
  const elements = new Map();
  const document = {
    getElementById(id) {
      if (!elements.has(id)) elements.set(id, new Element());
      return elements.get(id);
    },
    createElement() { return new Element(); }
  };
  const paths = [];
  const fetch = async (url) => {
    paths.push(url);
    const data = responses[url];
    return data ? {ok: true, json: async () => data} : {ok: false, status: 503};
  };
  vm.runInNewContext(source, {
    document, fetch, AbortController, Date,
    window: {setInterval() {}, setTimeout() { return 1; }, clearTimeout() {}}
  });
  await new Promise((resolve) => setImmediate(resolve));
  return {elements, paths, text: (id) => elements.get(id)?.textContent};
}

test("diagnostics renders live measurements and safely displays recent errors", async () => {
  const view = await render({
    "/api/v1/status": {status: "running", video: {fps: 29.8},
      detector: {enabled: true, fps: 17.2, last_inference_ms: 32.4}},
    "/api/v1/metrics": {rtsp: {health: "connected", codec: "H264", width: 1280,
      height: 720, transport: "tcp", last_frame_age_ms: 40, reconnect_count: 2,
      frames_dropped: 3, packets_lost: 4}, inference_queue: {depth: 1, dropped: 2,
      discarded: 1}, encoded_media_queue: {depth: 4, dropped: 0, discarded: 0},
      encoder_mode: "passthrough", websocket_clients: 3,
      whip: {peer_state: "connected", ice_state: "completed", access_units_sent: 92,
        media_queue_drops: 1, media_queue_discarded: 5},
      webrtc: {active_peers: 1, peers: [{session_id: "peer-1", peer_state: "connected",
        ice_state: "completed", selected_interface: "eth0",
        local_candidate: "192.0.2.5:5000"}]},
      recent_errors: [{timestamp: "2026-09-25T00:00:00Z", data: {
        module: "video", message: "<script>alert(1)</script>"}}]},
    "/api/v1/recordings": {available: true, state: "recording", active: true,
      access_units_written: 120},
    "/api/v1/gps": {available: true, source: "fixed", latitude: 25.03,
      longitude: 121.56}
  });
  assert.deepEqual(view.paths.sort(), ["/api/v1/gps", "/api/v1/metrics",
    "/api/v1/recordings", "/api/v1/status"]);
  assert.equal(view.text("source-health"), "Connected");
  assert.match(view.text("source-fps"), /29\.8/);
  assert.match(view.text("inference-latency"), /32\.4/);
  assert.equal(view.text("whip-state"), "Connected");
  assert.equal(view.text("websocket-count"), "3");
  assert.equal(view.text("recording-state"), "Recording");
  assert.match(view.text("gps-position"), /25\.03/);
  assert.equal(view.elements.get("peers").children.length, 1);
  assert.equal(view.elements.get("errors").children[0].textContent,
    "video · <script>alert(1)</script>");
});

test("diagnostics identifies missing telemetry", async () => {
  const view = await render({"/api/v1/status": {status: "degraded", video: {fps: null},
    detector: {enabled: false, fps: null, last_inference_ms: null}}});
  assert.equal(view.text("source-health"), "Unavailable");
  assert.equal(view.text("recording-state"), "Unavailable");
  assert.equal(view.text("gps-source"), "Unavailable");
  assert.equal(view.text("fetch-state"), "Some telemetry is unavailable");
});

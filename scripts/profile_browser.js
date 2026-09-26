function markerCrc(bytes) {
  let crc = 0;
  for (const byte of bytes) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit++) crc = ((crc << 1) ^ ((crc & 128) ? 7 : 0)) & 255;
  }
  return crc;
}

// Timestamp watermark format: magic(8), run nonce(32), Unix ms(48), CRC-8(8).
function decodeMarker(data, nonce) {
  const bytes = [];
  for (let i = 0; i < 96; i++) {
    const index = (i * 8 + 4) * 4;
    const level = (data[index] + data[index + 1] + data[index + 2]) / 3;
    if (!Number.isFinite(level) || (level > 80 && level < 175)) return null;
    if (i % 8 === 0) bytes.push(0);
    bytes[bytes.length - 1] = bytes[bytes.length - 1] * 2 + (level >= 128 ? 1 : 0);
  }
  if (bytes[0] !== 0xA5 || markerCrc(bytes.slice(0, 11)) !== bytes[11]) return null;
  const value = (start, end) => bytes.slice(start, end).reduce((a, b) => a * 256 + b, 0);
  return value(1, 5) === nonce ? value(5, 11) : null;
}

async function startProfile(nonce) {
  const video = document.getElementById('live-video');
  const canvas = document.createElement('canvas');
  const context = canvas.getContext('2d', {willReadFrequently: true});
  let frames = 0, invalid = 0, samples = [], closed = false;
  const probe = {count: 0, total_ms: 0};
  window.__profile = {
    async poll() {
      const ui = await window.skaiProfileSnapshot();
      const inbound = ui.stats.find(s => s.type === 'inbound-rtp' &&
        (s.kind === 'video' || s.mediaType === 'video'));
      const result = {frames, time: video.currentTime, width: video.videoWidth,
        height: video.videoHeight, clock_skew_ms: Date.now() - (performance.timeOrigin + performance.now()), state: ui.state, peer: ui.peer, annotation: ui.annotation,
        latency: samples, invalid, probe: {...probe}, stats: inbound ? {id: inbound.id,
          framesDecoded: inbound.framesDecoded, jitterBufferDelay: inbound.jitterBufferDelay,
          jitterBufferEmittedCount: inbound.jitterBufferEmittedCount,
          totalDecodeTime: inbound.totalDecodeTime} : null};
      samples = []; invalid = 0;
      return result;
    },
    async close() {
      closed = true;
    }
  };
  const observe = (now, metadata) => {
    if (closed) return;
    frames++;
    // Read only 3 KiB every fifth presented frame, and measure observer overhead.
    if (frames % 5 !== 0) { video.requestVideoFrameCallback(observe); return; }
    const started = performance.now();
    canvas.width = 768; canvas.height = 1;
    context.drawImage(video, 0, 16, 768, 1, 0, 0, 768, 1);
    const stamp = decodeMarker(context.getImageData(0, 0, 768, 1).data, nonce);
    const display = performance.timeOrigin + (metadata.expectedDisplayTime ?? now);
    const latency = stamp === null ? null : display - stamp;
    if (latency !== null && latency >= 0 && latency <= 5000) samples.push(latency);
    else invalid++;
    // Bound data between polls if the controller goes away.
    if (samples.length > 300) samples.shift();
    probe.count++; probe.total_ms += performance.now() - started;
    video.requestVideoFrameCallback(observe);
  };
  video.requestVideoFrameCallback(observe);
}
if (typeof module !== 'undefined') module.exports = {decodeMarker, markerCrc};

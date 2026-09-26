const assert = require('node:assert/strict');
const {decodeMarker, markerCrc} = require('../../scripts/profile_browser.js');
const nonce = 0xFEDCBA98, stamp = 1700000000123;
const payload = [0xA5, ...[24, 16, 8, 0].map(b => (nonce >>> b) & 255)];
for (let b = 40; b >= 0; b -= 8) payload.push(Math.floor(stamp / 2 ** b) % 256);
assert.equal(markerCrc([...Buffer.from('123456789')]), 0xF4);
payload.push(markerCrc(payload));
const pixels = new Uint8ClampedArray(768 * 4);
payload.forEach((byte, i) => {
  for (let b = 0; b < 8; b++) {
    const at = ((i * 8 + b) * 8 + 4) * 4;
    pixels.fill(byte & (1 << (7 - b)) ? 240 : 16, at, at + 3);
  }
});
assert.equal(decodeMarker(pixels, nonce), stamp);
assert.equal(decodeMarker(pixels, nonce - 1), null);
const flipped = pixels.slice(); flipped.fill(16, 4 * 4, 4 * 4 + 3);
assert.equal(decodeMarker(flipped, nonce), null);
const ambiguous = pixels.slice(); ambiguous.fill(120, 4 * 4, 4 * 4 + 3);
assert.equal(decodeMarker(ambiguous, nonce), null);
assert.equal(decodeMarker(new Uint8ClampedArray(), nonce), null);
// A pair of pixel bit errors must not turn into an apparently valid timestamp.
const doubleError = pixels.slice();
for (const cell of [10 * 8 + 7, 11 * 8 + 7]) {
  const at = (cell * 8 + 4) * 4;
  doubleError.fill(doubleError[at] > 128 ? 16 : 240, at, at + 3);
}
assert.equal(decodeMarker(doubleError, nonce), null);

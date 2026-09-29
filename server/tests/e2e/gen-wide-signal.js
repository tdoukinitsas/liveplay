// Writes a 48 kHz, 16-bit stereo signal with a KNOWN mid/side split, for the
// stereo-image checks.
//
// The main test signal is identical on both lanes. That is exactly right for
// everything else — and useless here, because identical lanes have no side
// content at all, so a width control has nothing to act on and every setting
// would measure the same. (That property is itself worth a test, and
// width-e2e.js makes it one.)
//
// So: two tones the lanes disagree about.
//
//     L = mid + side        mid  = 1 kHz    -> survives bass-mono
//     R = mid - side        side =   60 Hz  -> removed by bass-mono
//
// Equal amplitudes, and the two are at different frequencies so they are
// uncorrelated and their POWERS simply add. That makes every level assertion
// arithmetic rather than approximate:
//
//     width 0  -> mid only          -> -3.01 dB
//     width 1  -> mid + side        ->  reference
//     width 2  -> mid + 4x side     -> +3.98 dB
//
// and the same split drives the correlation readout, which is
// (mid - side) / (mid + side): +1 at width 0, 0 at width 1, -0.6 at width 2.
//
// The side tone is low so that ONE signal also exercises bass-mono, which
// should remove it and leave the 1 kHz mid alone.
//
// Amplitudes are 0.25 each, so L peaks at 0.5 (-6 dBFS) at unity and at 0.75
// (-2.5 dBFS) at double width. Any louder and the widest setting would clip
// the output before the assertion could read it.
const fs = require('fs');

const fs_hz = 48000, secs = 120, ch = 2, amp = 0.25;
const F_MID = 1000, F_SIDE = 60;
const n = fs_hz * secs;
const data = Buffer.alloc(n * ch * 2);
for (let i = 0; i < n; i++) {
  const mid  = amp * Math.sin(2 * Math.PI * F_MID  * i / fs_hz);
  const side = amp * Math.sin(2 * Math.PI * F_SIDE * i / fs_hz);
  data.writeInt16LE(Math.round((mid + side) * 32767), (i * ch) * 2);
  data.writeInt16LE(Math.round((mid - side) * 32767), (i * ch + 1) * 2);
}
const hdr = Buffer.alloc(44);
hdr.write('RIFF', 0);
hdr.writeUInt32LE(36 + data.length, 4);
hdr.write('WAVE', 8);
hdr.write('fmt ', 12);
hdr.writeUInt32LE(16, 16);
hdr.writeUInt16LE(1, 20);          // PCM
hdr.writeUInt16LE(ch, 22);
hdr.writeUInt32LE(fs_hz, 24);
hdr.writeUInt32LE(fs_hz * ch * 2, 28);
hdr.writeUInt16LE(ch * 2, 32);
hdr.writeUInt16LE(16, 34);
hdr.write('data', 36);
hdr.writeUInt32LE(data.length, 40);
fs.writeFileSync(process.argv[2], Buffer.concat([hdr, data]));
console.log('wrote', process.argv[2], hdr.length + data.length, 'bytes');

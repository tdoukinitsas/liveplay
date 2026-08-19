// Writes a 48 kHz, 16-bit stereo signal at a constant -6 dBFS peak, for the
// PFL end-to-end check: a steady peak means the monitor meter can be compared
// against a known number instead of "is it moving".
//
// It is a repeating SWEEP, not a fixed tone, for one specific assertion — that
// PFL and cue pre-listen sum in the monitor. Both sources are this same file,
// and with a fixed tone two instances are perfectly correlated: their sum
// depends on the arbitrary phase between two independently-started playbacks,
// which measured +3.9 dB one run and +0.6 dB the next. A sweep puts the two
// playback positions at different frequencies, so they add as uncorrelated
// signals and the sum is stable. The peak is unchanged, so every level
// assertion still reads -6 dBFS.
//
// Long enough to outlast the whole harness — the first version was three
// seconds, and every late assertion measured the file running out rather than
// the thing it was asking about.
const fs = require('fs');

const fs_hz = 48000, secs = 120, ch = 2, amp = 0.5;   // 0.5 = -6.02 dBFS
// A one-second cycle, so a measurement window of a whole number of seconds
// covers exactly the same spectral content wherever it happens to start.
//
// At five seconds it did not: a 1.2 s window caught a different slice of the
// sweep each time, and three measurements of an identical flat chain read
// -9.1, -9.0 and -8.4 dB. That looked like an EQ band failing to flatten and
// was purely the measurement moving underneath it.
const F_LO = 200, F_HI = 2000, SWEEP_SECS = 1;
const n = fs_hz * secs;
const data = Buffer.alloc(n * ch * 2);
let phase = 0;
for (let i = 0; i < n; i++) {
  // The frequency sweeps UP then back DOWN, a triangle rather than a sawtooth.
  //
  // Phase is accumulated either way, so there is never a phase discontinuity,
  // but a sawtooth still jumps 2 kHz -> 200 Hz once per cycle and that jump in
  // frequency is a broadband click. A measurement window that happened to
  // catch one read about 0.3 dB hot, reproducibly, which looked like an EQ
  // band failing to flatten and was nothing of the kind. A triangle has no
  // seam to catch.
  const t   = (i / fs_hz) % SWEEP_SECS;
  const ramp = t / SWEEP_SECS;                       // 0..1
  const tri  = ramp < 0.5 ? ramp * 2 : (1 - ramp) * 2;
  const f    = F_LO + (F_HI - F_LO) * tri;
  phase += 2 * Math.PI * f / fs_hz;
  const s = Math.round(amp * 32767 * Math.sin(phase));
  data.writeInt16LE(s, (i * ch) * 2);
  data.writeInt16LE(s, (i * ch + 1) * 2);
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

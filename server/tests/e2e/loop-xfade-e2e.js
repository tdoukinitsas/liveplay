// Loop crossfade (#56), end-to-end against a running server.
//
// The signal is 2 s of tone followed by 1 s of digital silence, looped from
// the top. What the meter should see, per pass:
//   loopCrossfade 0 -> the silent second plays in full every time round.
//   loopCrossfade 1 -> the last second (the silence) is blended with the first
//                      second (tone, fading up), then playback carries on
//                      from 1 s. The silence is never heard on its own; the
//                      longest quiet stretch is the first few ms of the blend.
// Measured on the cue's own source meter, so nothing downstream (bus DSP, the
// limiter) can make a wrong answer look right. The control case is what
// proves the measurement can see the silence at all.
//
// Also checks the playhead: with the blend, it never reports a position in
// the first second after the first pass, because the loop resumes at 1 s.
//
// usage: node loop-xfade-e2e.js <port>
const fs   = require('fs');
const os   = require('os');
const path = require('path');
const WebSocket = require(require.resolve('ws', { paths: [process.cwd()] }));

const PORT = process.argv[2] || '4480';
const BASE = `http://127.0.0.1:${PORT}`;
let failures = 0;
const ok = (name, pass, detail) => {
  console.log(`${pass ? 'PASS' : 'FAIL'}  ${name}   ${detail}`);
  if (!pass) failures++;
};
const sleep = ms => new Promise(r => setTimeout(r, ms));
async function rest(p, opts = {}) {
  const r = await fetch(BASE + p, { headers: { 'content-type': 'application/json' }, ...opts });
  const text = await r.text();
  let body; try { body = JSON.parse(text); } catch { body = text; }
  return { status: r.status, body };
}

function writeWav(file) {
  const rate = 48000, ch = 2, n = rate * 3;
  const data = Buffer.alloc(n * ch * 2);
  for (let i = 0; i < rate * 2; i++) {
    const v = Math.round(Math.sin(2 * Math.PI * 440 * i / rate) * 0.5 * 32767);
    data.writeInt16LE(v, i * 4); data.writeInt16LE(v, i * 4 + 2);
  }
  const h = Buffer.alloc(44);
  h.write('RIFF', 0); h.writeUInt32LE(36 + data.length, 4); h.write('WAVE', 8);
  h.write('fmt ', 12); h.writeUInt32LE(16, 16); h.writeUInt16LE(1, 20); h.writeUInt16LE(ch, 22);
  h.writeUInt32LE(rate, 24); h.writeUInt32LE(rate * ch * 2, 28); h.writeUInt16LE(ch * 2, 32);
  h.writeUInt16LE(16, 34); h.write('data', 36); h.writeUInt32LE(data.length, 40);
  fs.writeFileSync(file, Buffer.concat([h, data]));
}

// Play one looping item for `ms`, recording every per-cue meter frame.
async function measure(ws, uuid, ms) {
  const frames = [];
  const onMsg = raw => {
    let m; try { m = JSON.parse(raw); } catch { return; }
    if (m.type !== 'meters') return;
    const list = m.items || m.item_meters || m.cues || [];
    for (const it of list) {
      if (!it.sources) continue;
      const peak = Math.max(...it.sources.map(s => s.peak_max_db ?? s.peak_db ?? -200));
      frames.push({ t: Date.now(), peak, playhead: it.playhead_seconds });
    }
  };
  ws.on('message', onMsg);
  await rest(`/api/project/items/${uuid}/play`, { method: 'POST', body: '{}' });
  await sleep(ms);
  await rest(`/api/project/items/${uuid}/stop`, { method: 'POST', body: JSON.stringify({ fade_ms: 0 }) });
  ws.off('message', onMsg);
  // Longest run of consecutive quiet frames, in ms.
  let longest = 0, runStart = null;
  for (const f of frames) {
    if (f.peak < -50) { if (runStart === null) runStart = f.t; longest = Math.max(longest, f.t - runStart); }
    else runStart = null;
  }
  return { frames, longestQuietMs: longest };
}

(async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'lp-loopx-'));
  const a = path.join(dir, 'a.wav'); writeWav(a);
  const b = path.join(dir, 'b.wav'); writeWav(b);
  const loop = { action: 'loop' };
  let r = await rest('/api/project/document', { method: 'PUT', body: JSON.stringify({
    name: 'loop-xfade-e2e',
    items: [
      { uuid: 'lx-plain', type: 'audio', displayName: 'Plain', mediaServerPath: a,
        endBehavior: loop, loopCrossfade: 0, manualStopFade: 0, fadeOutDuration: 0 },
      { uuid: 'lx-blend', type: 'audio', displayName: 'Blend', mediaServerPath: b,
        endBehavior: loop, loopCrossfade: 1.0, manualStopFade: 0, fadeOutDuration: 0 },
    ],
  }) });
  ok('loaded the test project', r.status === 200, `status ${r.status}`);
  for (let i = 0; i < 100; i++) {
    if ((await rest('/api/project/progress')).body.loading === false) break;
    await sleep(100);
  }

  const ws = new WebSocket(`ws://127.0.0.1:${PORT}/ws`);
  await new Promise((res, rej) => { ws.once('open', res); ws.once('error', rej); });

  const plain = await measure(ws, 'lx-plain', 7000);
  ok('meter frames arrived for the plain loop', plain.frames.length > 50, `${plain.frames.length} frames`);
  ok('control: without a crossfade the silent second is heard',
     plain.longestQuietMs >= 700, `longest quiet run ${plain.longestQuietMs} ms`);
  ok('control: the plain loop kept playing (it wrapped)',
     plain.frames.some((f, i) => i > 0 && f.playhead < plain.frames[i - 1].playhead),
     'playhead went backwards at least once');

  // The first play starts the head decode; give it a moment on a slow disk.
  const blend = await measure(ws, 'lx-blend', 7000);
  ok('meter frames arrived for the blended loop', blend.frames.length > 50, `${blend.frames.length} frames`);
  ok('with a 1 s loop crossfade the silence is never heard on its own',
     blend.longestQuietMs < 150, `longest quiet run ${blend.longestQuietMs} ms`);
  const afterFirstWrap = blend.frames.slice(blend.frames.findIndex((f, i) =>
    i > 0 && f.playhead < blend.frames[i - 1].playhead));
  const minAfter = Math.min(...afterFirstWrap.map(f => f.playhead));
  ok('after turning round it resumes past the blended head (>= 1 s)',
     afterFirstWrap.length > 0 && minAfter >= 0.95, `min playhead after wrap ${minAfter.toFixed(3)} s`);
  const maxAll = Math.max(...blend.frames.map(f => f.playhead));
  ok('the playhead never runs past the end of the file', maxAll <= 3.01, `max ${maxAll.toFixed(3)} s`);

  ws.close();
  await rest('/api/project/close', { method: 'POST', body: '{}' });
  fs.rmSync(dir, { recursive: true, force: true });
  console.log(failures ? `\nFAILURES: ${failures}` : '\nALL PASS');
  process.exit(failures ? 1 : 0);
})().catch(e => { console.error(e); process.exit(2); });

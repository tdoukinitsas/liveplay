// EQ beyond four bands, the analyser, and the dynamics level readings,
// end-to-end on a running server with real audio through a bus.
//
//   A. A new bus still starts with the classic four bells.
//   B. A FIFTH band works: a high cut added at index 4 takes the sweep down;
//      switching the extra bands off trims them and puts the level back.
//   C. Band types round-trip (notch on an inner band; the legacy `shelf` flag
//      is covered by filters-e2e).
//   D. dyn_in_db / dyn_out_db ride the meters frame: the input reads the
//      signal even with the dynamics switched out, and with 4:1 at -18 dB the
//      output sits about 9 dB under the input.
//   E. The analyser: set_analyser subscribes, frames arrive with 96 bins
//      before and after the EQ, a -18 dB bell at 1 kHz shows up as a dip in
//      the post spectrum only, and unsubscribing stops the frames.
//
// usage: node eq-analyser-e2e.js <port> <wavPath>   (gen-signal.js's sweep)
const WebSocket = require(require.resolve('ws', { paths: [process.cwd()] }));
const PORT = process.argv[2] || '4480';
const WAV  = process.argv[3];
const BASE = `http://127.0.0.1:${PORT}`;
let failures = 0;
const ok = (n, p, d = '') => { console.log(`${p ? 'PASS' : 'FAIL'}  ${n}${d ? '   ' + d : ''}`); if (!p) failures++; };
const rest = async (p, o = {}) => {
  const r = await fetch(BASE + p, { headers: { 'content-type': 'application/json' }, ...o });
  const t = await r.text();
  try { return { status: r.status, body: JSON.parse(t) }; } catch { return { status: r.status, body: t }; }
};
const sleep = ms => new Promise(r => setTimeout(r, ms));
const SETTLE_MS = 1500;

class Frames {
  constructor(ws) {
    this.meters = []; this.analyser = []; this.subscribed = null;
    ws.on('message', raw => {
      let m; try { m = JSON.parse(raw); } catch { return; }
      if (m.type === 'meters') this.meters.push(m);
      else if (m.type === 'analyser') this.analyser.push(m);
      else if (m.type === 'analyser_subscribed') this.subscribed = m;
    });
  }
  reset() { this.meters = []; this.analyser = []; }
  chans(id) { return this.meters.flatMap(f => (f.mixer_channels || []).filter(c => c.mixer_id === id)); }
  rms(id) {
    let sum = 0, n = 0;
    for (const c of this.chans(id)) if ((c.rms_db ?? -200) > -190) { sum += Math.pow(10, c.rms_db / 10); n++; }
    return n ? 10 * Math.log10(sum / n) : -200;
  }
  max(id, key) { return Math.max(-200, ...this.chans(id).map(c => c[key] ?? -200)); }
}
const measure = async (f, ms) => { f.reset(); await sleep(ms); };

(async () => {
  const uuid = 'item-eq-0001';
  await rest('/api/project/document', { method: 'PUT', body: JSON.stringify({
    name: 'eq-analyser', items: [{ uuid, type: 'audio', displayName: 'Sweep', mediaServerPath: WAV,
                                   volume: 1, endBehavior: { action: 'loop' } }] }) });
  for (let i = 0; i < 80; i++) {
    if ((await rest('/api/project/progress')).body.loading === false) break;
    await sleep(100);
  }
  const busId = (await rest('/api/buses', { method: 'POST', body: JSON.stringify({ name: 'EQ test', width: 2 }) })).body.id;
  await rest(`/api/project/items/${uuid}`, { method: 'PATCH', body: JSON.stringify({ busId }) });
  const getBus = async () => (await rest(`/api/buses/${busId}`)).body;
  const patchDsp = dsp => rest(`/api/buses/${busId}`, { method: 'PATCH', body: JSON.stringify({ dsp }) });
  let b = await getBus();

  // ---- A ------------------------------------------------------------------
  ok('a new bus has four bands', b.dsp.eq.length === 4, `${b.dsp.eq.length}`);
  ok('...all bells, all on, at the classic frequencies',
     JSON.stringify(b.dsp.eq.map(e => [e.type, e.on, e.freq])) ===
     JSON.stringify([['bell', true, 100], ['bell', true, 500], ['bell', true, 2500], ['bell', true, 10000]]),
     JSON.stringify(b.dsp.eq.map(e => [e.type, e.on, e.freq])));

  const ws = new WebSocket(`ws://127.0.0.1:${PORT}/ws`);
  await new Promise((res, rej) => { ws.on('open', res); ws.on('error', rej); });
  const f = new Frames(ws);
  await rest(`/api/project/items/${uuid}/play`, { method: 'POST', body: '{}' });
  await sleep(SETTLE_MS);
  await measure(f, 1500);
  const open = f.rms(b.mixerId);
  ok('the sweep reaches the bus', open > -30, `${open.toFixed(1)} dB RMS`);

  // ---- D (dynamics off) ---------------------------------------------------
  const inOff = f.max(b.mixerId, 'dyn_in_db');
  ok('dyn_in_db reads the signal with the dynamics switched out', Math.abs(inOff - -6) < 1.5,
     `${inOff.toFixed(1)} dBFS (signal is -6)`);

  // ---- B ------------------------------------------------------------------
  const base = b.dsp.eq.map(e => ({ ...e }));
  await patchDsp({ eq: [...base, { type: 'highCut', freq: 300, q: 0.707, gain: 0, on: true },
                                 { type: 'bell', freq: 1000, gain: 0, q: 1, on: true }] });
  b = await getBus();
  ok('two bands can be added', b.dsp.eq.length === 6, `${b.dsp.eq.length}`);
  ok('...with their types', b.dsp.eq[4].type === 'highCut' && b.dsp.eq[5].type === 'bell',
     JSON.stringify(b.dsp.eq.slice(4).map(e => e.type)));
  await sleep(SETTLE_MS);
  await measure(f, 1500);
  const cut = f.rms(b.mixerId);
  ok('the fifth band is live: a 300 Hz high cut takes the 200 Hz-2 kHz sweep down', open - cut > 8,
     `${open.toFixed(1)} -> ${cut.toFixed(1)} dB RMS`);

  // ---- E ------------------------------------------------------------------
  // Band 5 (index 5) becomes a deep 1 kHz bell; the cut goes off. Analyser on.
  await patchDsp({ eq: [...base, { on: false }, { type: 'bell', freq: 1000, gain: -18, q: 4, on: true }] });
  ws.send(JSON.stringify({ type: 'set_analyser', busId }));
  await sleep(SETTLE_MS);
  await measure(f, 3000);
  ok('set_analyser is acknowledged', f.subscribed && f.subscribed.busId === busId, JSON.stringify(f.subscribed));
  ok('analyser frames arrive', f.analyser.length > 20, `${f.analyser.length} frames in 3 s`);
  const frame = f.analyser[0] || {};
  ok('...with 96 bins before and after the EQ', frame.pre?.length === 96 && frame.post?.length === 96,
     `${frame.pre?.length}/${frame.post?.length}`);
  const hold = key => { const out = new Array(96).fill(-200);
    for (const fr of f.analyser) fr[key].forEach((v, i) => { out[i] = Math.max(out[i], v); }); return out; };
  const pre = hold('pre'), post = hold('post');
  const binOf = hz => Math.floor(96 * Math.log(hz / 20) / Math.log(20000 / 20));
  const d1k  = post[binOf(1000)] - pre[binOf(1000)];
  const d300 = post[binOf(300)]  - pre[binOf(300)];
  ok('the 1 kHz cut shows in the post-EQ spectrum', d1k < -10, `${d1k.toFixed(1)} dB at 1 kHz`);
  ok('...and nowhere near it', Math.abs(d300) < 3, `${d300.toFixed(1)} dB at 300 Hz`);
  ok('the pre-EQ spectrum sees the sweep', pre[binOf(1000)] > -30, `${pre[binOf(1000)].toFixed(1)} dBFS`);

  ws.send(JSON.stringify({ type: 'set_analyser', busId: null }));
  await sleep(500);
  await measure(f, 800);
  ok('unsubscribing stops the frames', f.analyser.length === 0, `${f.analyser.length} frames`);

  // Switch the extra bands off: trimmed, level restored.
  await patchDsp({ eq: [...base, { on: false }, { on: false }] });
  b = await getBus();
  ok('switched-off trailing bands are trimmed', b.dsp.eq.length === 4, `${b.dsp.eq.length}`);
  await sleep(SETTLE_MS);
  await measure(f, 1500);
  ok('...and the level is back', Math.abs(f.rms(b.mixerId) - open) < 0.5,
     `${open.toFixed(1)} vs ${f.rms(b.mixerId).toFixed(1)} dB RMS`);

  // ---- C ------------------------------------------------------------------
  await patchDsp({ eq: [{}, { type: 'notch', freq: 1000, q: 2 }] });
  b = await getBus();
  ok('any band can take any type (a notch on band 2)', b.dsp.eq[1].type === 'notch' && b.dsp.eq[1].shelf === false,
     JSON.stringify(b.dsp.eq[1]));
  await patchDsp({ eq: [{}, { type: 'bell', freq: 500, q: 1 }] });

  // ---- D (compressing) ----------------------------------------------------
  await patchDsp({ comp: { on: true, threshold: -18, ratio: 4, knee: 0, makeup: 0, attack: 1, release: 200 } });
  await sleep(SETTLE_MS);
  await measure(f, 1500);
  const inC = f.max(b.mixerId, 'dyn_in_db'), outC = f.max(b.mixerId, 'dyn_out_db');
  ok('with 4:1 at -18 the dynamics output sits ~9 dB under the input', Math.abs((inC - outC) - 9) < 1.5,
     `in ${inC.toFixed(1)}, out ${outC.toFixed(1)} dBFS`);
  const gr = Math.min(0, ...f.chans(b.mixerId).map(c => c.comp_gr_db ?? 0));
  ok('comp_gr_db still reports the reduction', Math.abs(gr + 9) < 1.5, `${gr.toFixed(1)} dB`);

  ws.close();
  await rest(`/api/project/items/${uuid}/stop`, { method: 'POST', body: JSON.stringify({ fade_ms: 0 }) });
  await rest('/api/project/close', { method: 'POST', body: '{}' });
  console.log(failures ? `\nFAILURES: ${failures}` : '\nALL PASS');
  process.exit(failures ? 1 : 0);
})().catch(e => { console.error(e); process.exit(2); });

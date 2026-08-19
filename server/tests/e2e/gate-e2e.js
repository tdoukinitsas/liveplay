// Does the channel gate actually gate, end to end?
//
// The unit tests prove the processor in isolation. This proves the path: REST
// -> bus definition -> engine coefficients -> the render thread -> the bus
// meter, with real audio running through it.
//
// The test signal is a steady sweep at -6 dBFS peak, so the gate is driven by
// changing its THRESHOLD rather than the signal: park the threshold below the
// material and it passes, lift it above and the gate shuts.
//
// usage: node gate-e2e.js <port> <wavPath>
const WebSocket = require(require.resolve('ws', { paths: [process.cwd()] }));
const PORT = process.argv[2], WAV = process.argv[3];
const BASE = `http://127.0.0.1:${PORT}`;

let failures = 0;
const ok = (n, p, d = '') => { console.log(`${p ? 'PASS' : 'FAIL'}  ${n}${d ? '   ' + d : ''}`); if (!p) failures++; };
const rest = async (p, o = {}) => {
  const r = await fetch(BASE + p, { headers: { 'content-type': 'application/json' }, ...o });
  const t = await r.text();
  try { return { status: r.status, body: JSON.parse(t) }; } catch { return { status: r.status, body: t }; }
};
const sleep = ms => new Promise(r => setTimeout(r, ms));

// Same settle as the filter harness, and for the same reasons: the processor
// has its own attack and release, and the bus meter integrates on top of that.
const SETTLE_MS = 1500;

class Meters {
  constructor(ws) { this.f = []; ws.on('message', raw => {
    let m; try { m = JSON.parse(raw); } catch { return; }
    if (m.type === 'meters') this.f.push(m); }); }
  reset() { this.f = []; }
  rms(id) {
    let sum = 0, n = 0;
    for (const fr of this.f) for (const c of (fr.mixer_channels || []))
      if (c.mixer_id === id && (c.rms_db ?? -200) > -190) { sum += Math.pow(10, c.rms_db / 10); n++; }
    return n ? 10 * Math.log10(sum / n) : -200;
  }
  // Deepest gain reduction the gate reported over the window.
  gr(id) {
    let worst = 0;
    for (const fr of this.f) for (const c of (fr.mixer_channels || []))
      if (c.mixer_id === id && typeof c.gate_gr_db === 'number')
        worst = Math.min(worst, c.gate_gr_db);
    return worst;
  }
}
const measure = async (m, ms) => { m.reset(); await sleep(ms); };

const uuid = 'item-gate-0001';
const gate = (o) => ({ gate: { on: true, ratio: 10, range: -40, attack: 1,
                               hold: 10, release: 50, ...o } });

// The "shut" thresholds below are 0 dB, not -3. The signal is -6 dBFS peak and
// the gate closes 3 dB below its threshold, so -3 puts the close point at
// exactly -6.0 - the signal's own level, inside the hysteresis window. The gate
// correctly held open there; the test was reading it as a failure to gate.

(async () => {
  await rest('/api/project/document', {
    method: 'PUT',
    body: JSON.stringify({ name: 'gate', items: [{
      uuid, type: 'audio', displayName: 'Sweep', mediaServerPath: WAV,
      volume: 1, endBehavior: 'stop' }] }),
  });
  for (let i = 0; i < 80; i++) {
    const p = await rest('/api/project/progress');
    if (p.body && p.body.loading === false) break;
    await sleep(100);
  }

  const bus = (await rest('/api/buses', {
    method: 'POST', body: JSON.stringify({ name: 'Gated', width: 2 }) })).body.id;
  await rest(`/api/project/items/${uuid}`, {
    method: 'PATCH', body: JSON.stringify({ busId: bus }) });

  const b = (await rest('/api/buses')).body.find(x => x.id === bus);
  ok('the bus reports gate settings', !!b.dsp.gate, JSON.stringify(b.dsp.gate));
  ok('the gate starts switched out', b.dsp.gate.on === false);

  const ws = new WebSocket(`ws://127.0.0.1:${PORT}/ws`);
  await new Promise((res, rej) => { ws.on('open', res); ws.on('error', rej); });
  const m = new Meters(ws);

  await rest(`/api/project/items/${uuid}/play`, { method: 'POST', body: '{}' });
  await sleep(SETTLE_MS);
  await measure(m, 2000);
  const open = m.rms(b.mixerId);
  ok('signal is reaching the bus', open > -40, `${open.toFixed(1)} dB RMS`);
  ok('an inactive gate reports no reduction', m.gr(b.mixerId) === 0,
     `${m.gr(b.mixerId).toFixed(1)} dB`);

  // ---- Threshold below the material: passes ----
  await rest(`/api/buses/${bus}/dsp`, {
    method: 'POST', body: JSON.stringify(gate({ threshold: -60 })) });
  await sleep(SETTLE_MS);
  await measure(m, 2000);
  ok('an open gate is transparent', Math.abs(m.rms(b.mixerId) - open) < 0.3,
     `${open.toFixed(1)} -> ${m.rms(b.mixerId).toFixed(1)} dB RMS`);
  ok('and reports no reduction while open', m.gr(b.mixerId) > -0.5,
     `${m.gr(b.mixerId).toFixed(1)} dB`);

  // ---- Threshold above the material: shuts ----
  await rest(`/api/buses/${bus}/dsp`, {
    method: 'POST', body: JSON.stringify(gate({ threshold: 0 })) });
  await sleep(SETTLE_MS);
  await measure(m, 2000);
  const shut = m.rms(b.mixerId);
  ok('a gate above the material shuts it down', shut < open - 20,
     `${open.toFixed(1)} -> ${shut.toFixed(1)} dB RMS`);
  ok('and the meter reports the reduction', m.gr(b.mixerId) < -20,
     `${m.gr(b.mixerId).toFixed(1)} dB`);

  // ---- Range limits how far it can pull down ----
  await rest(`/api/buses/${bus}/dsp`, {
    method: 'POST', body: JSON.stringify(gate({ threshold: -3, range: -6 })) });
  await sleep(SETTLE_MS);
  await measure(m, 2000);
  const ranged = m.rms(b.mixerId);
  ok('range floors the attenuation at 6 dB', Math.abs((open - ranged) - 6) < 1.0,
     `${open.toFixed(1)} -> ${ranged.toFixed(1)} dB RMS`);

  // ---- The section bypass takes it out ----
  await rest(`/api/buses/${bus}/dsp`, {
    method: 'POST', body: JSON.stringify({ ...gate({ threshold: 0 }), dynEnabled: false }) });
  await sleep(SETTLE_MS);
  await measure(m, 2000);
  ok('the dynamics bypass takes the gate out',
     Math.abs(m.rms(b.mixerId) - open) < 0.3,
     `${open.toFixed(1)} -> ${m.rms(b.mixerId).toFixed(1)} dB RMS`);

  // ...and putting the section back restores it.
  //
  // The gate settings are re-sent rather than assumed. /dsp is the in-gesture
  // path: it merges onto the STORED bus and deliberately writes nothing, so
  // nothing sent through it so far has been persisted. A lone dynEnabled here
  // would re-enable a section whose stored gate is still switched out, which
  // is what it did.
  await rest(`/api/buses/${bus}/dsp`, {
    method: 'POST', body: JSON.stringify({ ...gate({ threshold: 0 }), dynEnabled: true }) });
  await sleep(SETTLE_MS);
  await measure(m, 2000);
  ok('switching the section back in restores the gating',
     m.rms(b.mixerId) < open - 20,
     `${open.toFixed(1)} -> ${m.rms(b.mixerId).toFixed(1)} dB RMS`);

  // ---- Persistence ----
  await rest(`/api/buses/${bus}`, {
    method: 'PATCH',
    body: JSON.stringify({ dsp: gate({ threshold: -25, ratio: 8, hold: 120 }) }) });
  const saved = (await rest('/api/buses')).body.find(x => x.id === bus);
  ok('PATCH persists the gate settings',
     saved.dsp.gate.on === true && saved.dsp.gate.threshold === -25 &&
     saved.dsp.gate.ratio === 8 && saved.dsp.gate.hold === 120,
     JSON.stringify(saved.dsp.gate));

  const doc = (await rest('/api/project')).body;
  await rest('/api/project/document', {
    method: 'PUT',
    body: JSON.stringify({ name: doc.name, items: doc.items,
                           cartItems: doc.cartItems ?? [],
                           settings: doc.settings, theme: doc.theme }) });
  const after = (await rest('/api/buses')).body.find(x => x.id === bus);
  ok('the gate survives a document round trip',
     after && after.dsp.gate.threshold === -25 && after.dsp.gate.ratio === 8,
     after ? JSON.stringify(after.dsp.gate) : '(bus missing)');

  await rest(`/api/project/items/${uuid}/stop`, { method: 'POST', body: '{}' });
  ws.close();
  console.log(`\n${failures === 0 ? 'ALL PASS' : 'FAILURES'} (${failures})`);
  process.exit(failures === 0 ? 0 : 1);
})().catch(e => { console.error('harness error:', e); process.exit(2); });

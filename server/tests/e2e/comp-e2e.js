// Does the channel compressor actually compress, end to end?
//
// The unit tests prove the processor in isolation. This proves the path: REST
// -> bus definition -> engine coefficients -> the render thread -> the bus
// meter, with real audio running through it. It is the same shape as
// gate-e2e.js and exists for the same reason: the gate's coefficients were once
// published into a slot the render thread never read, and every unit test
// passed throughout.
//
// The test signal is a steady sweep, so the compressor is driven by moving its
// THRESHOLD relative to the material rather than by changing the signal.
//
// The expected reductions are computed from the peak level MEASURED at the bus,
// not from an assumed -6 dBFS. An assumption there turns every later assertion
// into a test of the wav file.
//
// usage: node comp-e2e.js <port> <wavPath>
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

// Same settle as the other dynamics harness, and for the same reasons: the
// processor has its own attack and release, and the bus meter integrates on top
// of that. The release below is deliberately long, so this is generous.
const SETTLE_MS = 2000;

class Meters {
  constructor(ws) { this.f = []; ws.on('message', raw => {
    let m; try { m = JSON.parse(raw); } catch { return; }
    if (m.type === 'meters') this.f.push(m); }); }
  reset() { this.f = []; }
  // Average POWER over the window, converted once at the end. Averaging the
  // decibel readings themselves is a geometric mean and weights quiet frames
  // far too heavily.
  rms(id) {
    let sum = 0, n = 0;
    for (const fr of this.f) for (const c of (fr.mixer_channels || []))
      if (c.mixer_id === id && (c.rms_db ?? -200) > -190) { sum += Math.pow(10, c.rms_db / 10); n++; }
    return n ? 10 * Math.log10(sum / n) : -200;
  }
  // peak_max_db, NOT peak_db. The compressor's detector is a raw sample peak,
  // and peak_max_db is the same quantity — the maximum since the last read,
  // with no ballistics, so no transient can be missed however slowly this
  // polls. peak_db is ballistically released and would be comparing a threshold
  // against a different measurement. On this constant-amplitude sweep the two
  // happen to agree, which is the reason to get it right now rather than when
  // a test uses a signal where they do not.
  peak(id) {
    let top = -200;
    for (const fr of this.f) for (const c of (fr.mixer_channels || []))
      if (c.mixer_id === id && typeof c.peak_max_db === 'number')
        top = Math.max(top, c.peak_max_db);
    return top;
  }
  // Deepest gain reduction the compressor reported over the window.
  gr(id) {
    let worst = 0;
    for (const fr of this.f) for (const c of (fr.mixer_channels || []))
      if (c.mixer_id === id && typeof c.comp_gr_db === 'number')
        worst = Math.min(worst, c.comp_gr_db);
    return worst;
  }
}
const measure = async (m, ms) => { m.reset(); await sleep(ms); };

const uuid = 'item-comp-0001';

// Fast attack, moderately slow release. Not a musical setting — a deliberate
// one: a release far longer than the sweep's period makes the smoothed gain sit
// at the DEEPEST reduction the detector reaches rather than wandering with the
// sweep, so a steady-state reading has a single arithmetic answer to check
// against.
//
// 200 ms and not the 1000 it started at, because the release is also how long
// this harness has to WAIT. Only transitions out of compression are slow — the
// attack is 1 ms — and at 1000 ms a step that dropped from 9 dB of reduction
// back to none was still 0.6 dB down when the window opened, which read as a
// hard knee doing 0.7 dB of work it should not have been doing. SETTLE_MS has
// to stay comfortably longer than a few of these time constants.
const comp = (o) => ({ comp: { on: true, ratio: 4, knee: 0, makeup: 0,
                               attack: 1, release: 200, ...o } });

(async () => {
  await rest('/api/project/document', {
    method: 'PUT',
    body: JSON.stringify({ name: 'comp', items: [{
      uuid, type: 'audio', displayName: 'Sweep', mediaServerPath: WAV,
      volume: 1, endBehavior: 'stop' }] }),
  });
  for (let i = 0; i < 80; i++) {
    const p = await rest('/api/project/progress');
    if (p.body && p.body.loading === false) break;
    await sleep(100);
  }

  const bus = (await rest('/api/buses', {
    method: 'POST', body: JSON.stringify({ name: 'Squashed', width: 2 }) })).body.id;
  await rest(`/api/project/items/${uuid}`, {
    method: 'PATCH', body: JSON.stringify({ busId: bus }) });

  const b = (await rest('/api/buses')).body.find(x => x.id === bus);
  ok('the bus reports compressor settings', !!b.dsp.comp, JSON.stringify(b.dsp.comp));
  ok('the compressor starts switched out', b.dsp.comp.on === false);

  const ws = new WebSocket(`ws://127.0.0.1:${PORT}/ws`);
  await new Promise((res, rej) => { ws.on('open', res); ws.on('error', rej); });
  const m = new Meters(ws);

  await rest(`/api/project/items/${uuid}/play`, { method: 'POST', body: '{}' });
  await sleep(SETTLE_MS);
  await measure(m, 2000);
  const open = m.rms(b.mixerId);
  const peak = m.peak(b.mixerId);
  ok('signal is reaching the bus', open > -40, `${open.toFixed(1)} dB RMS`);
  ok('and the peak is measurable', peak > -30, `${peak.toFixed(1)} dB peak`);
  ok('an inactive compressor reports no reduction', m.gr(b.mixerId) === 0,
     `${m.gr(b.mixerId).toFixed(1)} dB`);

  // ---- Threshold above the material: transparent ----
  await rest(`/api/buses/${bus}/dsp`, {
    method: 'POST', body: JSON.stringify(comp({ threshold: peak + 12 })) });
  await sleep(SETTLE_MS);
  await measure(m, 2000);
  ok('a compressor above the material is transparent',
     Math.abs(m.rms(b.mixerId) - open) < 0.3,
     `${open.toFixed(1)} -> ${m.rms(b.mixerId).toFixed(1)} dB RMS`);
  ok('and reports no reduction', m.gr(b.mixerId) > -0.5,
     `${m.gr(b.mixerId).toFixed(1)} dB`);

  // ---- 12 dB over at 4:1 is 9 dB of reduction ----
  await rest(`/api/buses/${bus}/dsp`, {
    method: 'POST', body: JSON.stringify(comp({ threshold: peak - 12 })) });
  await sleep(SETTLE_MS);
  await measure(m, 2000);
  const squashed = m.rms(b.mixerId);
  ok('12 dB over at 4:1 pulls down by 9 dB',
     Math.abs((open - squashed) - 9) < 1.2,
     `${open.toFixed(1)} -> ${squashed.toFixed(1)} dB RMS`);
  ok('and the GR meter agrees', Math.abs(m.gr(b.mixerId) + 9) < 1.2,
     `${m.gr(b.mixerId).toFixed(1)} dB`);

  // ---- Makeup gain puts it back ----
  await rest(`/api/buses/${bus}/dsp`, {
    method: 'POST', body: JSON.stringify(comp({ threshold: peak - 12, makeup: 9 })) });
  await sleep(SETTLE_MS);
  await measure(m, 2000);
  ok('9 dB of makeup restores the level',
     Math.abs(m.rms(b.mixerId) - open) < 1.2,
     `${squashed.toFixed(1)} -> ${m.rms(b.mixerId).toFixed(1)} dB RMS`);
  // The meter answers "how hard is it working", so makeup must not appear in it.
  ok('but makeup stays out of the GR meter', Math.abs(m.gr(b.mixerId) + 9) < 1.2,
     `${m.gr(b.mixerId).toFixed(1)} dB`);

  // ---- The knee, which is the part the notes leave out ----
  // With the threshold sitting ON the material a hard knee does nothing at all,
  // and a wide knee is already working. Run as a pair, because either reading
  // alone would also be explained by the threshold simply being in the wrong
  // place.
  await rest(`/api/buses/${bus}/dsp`, {
    method: 'POST', body: JSON.stringify(comp({ threshold: peak, knee: 0 })) });
  await sleep(SETTLE_MS);
  await measure(m, 2000);
  ok('a HARD knee is transparent at the threshold',
     Math.abs(m.rms(b.mixerId) - open) < 0.5,
     `${open.toFixed(1)} -> ${m.rms(b.mixerId).toFixed(1)} dB RMS`);

  // slope * (W/2)^2 / 2W = 0.75 * 144 / 48 = 2.25 dB at the threshold itself.
  await rest(`/api/buses/${bus}/dsp`, {
    method: 'POST', body: JSON.stringify(comp({ threshold: peak, knee: 24 })) });
  await sleep(SETTLE_MS);
  await measure(m, 2000);
  const kneed = m.rms(b.mixerId);
  ok('a 24 dB knee is already working at the threshold',
     Math.abs((open - kneed) - 2.25) < 1.0,
     `${open.toFixed(1)} -> ${kneed.toFixed(1)} dB RMS`);

  // ---- The limiter end of the ratio control ----
  await rest(`/api/buses/${bus}/dsp`, {
    method: 'POST', body: JSON.stringify(comp({ threshold: peak - 20, ratio: 60 })) });
  await sleep(SETTLE_MS);
  await measure(m, 2000);
  ok('60:1 holds 20 dB of overshoot to under 1 dB',
     Math.abs((open - m.rms(b.mixerId)) - 19.67) < 1.2,
     `${open.toFixed(1)} -> ${m.rms(b.mixerId).toFixed(1)} dB RMS`);

  // ---- The section bypass takes it out ----
  await rest(`/api/buses/${bus}/dsp`, {
    method: 'POST',
    body: JSON.stringify({ ...comp({ threshold: peak - 12 }), dynEnabled: false }) });
  await sleep(SETTLE_MS);
  await measure(m, 2000);
  ok('the dynamics bypass takes the compressor out',
     Math.abs(m.rms(b.mixerId) - open) < 0.3,
     `${open.toFixed(1)} -> ${m.rms(b.mixerId).toFixed(1)} dB RMS`);

  // ...and putting the section back restores it. The compressor settings are
  // re-sent rather than assumed: /dsp is the in-gesture path, it merges onto the
  // STORED bus and deliberately writes nothing, so a lone dynEnabled would
  // re-enable a section whose stored compressor is still switched out.
  await rest(`/api/buses/${bus}/dsp`, {
    method: 'POST',
    body: JSON.stringify({ ...comp({ threshold: peak - 12 }), dynEnabled: true }) });
  await sleep(SETTLE_MS);
  await measure(m, 2000);
  ok('switching the section back in restores the compression',
     Math.abs((open - m.rms(b.mixerId)) - 9) < 1.2,
     `${open.toFixed(1)} -> ${m.rms(b.mixerId).toFixed(1)} dB RMS`);

  // ---- The two processors coexist ----
  // Both in at once, with the gate parked open. The compressor must still be
  // doing exactly what it did on its own — a gate that is passing everything
  // must not change the answer.
  await rest(`/api/buses/${bus}/dsp`, {
    method: 'POST', body: JSON.stringify({
      ...comp({ threshold: peak - 12 }),
      gate: { on: true, threshold: -80, ratio: 10, range: -40,
              attack: 1, hold: 10, release: 50 },
    }) });
  await sleep(SETTLE_MS);
  await measure(m, 2000);
  ok('an open gate in front changes nothing',
     Math.abs((open - m.rms(b.mixerId)) - 9) < 1.2,
     `${open.toFixed(1)} -> ${m.rms(b.mixerId).toFixed(1)} dB RMS`);

  // ---- Persistence ----
  await rest(`/api/buses/${bus}`, {
    method: 'PATCH',
    body: JSON.stringify({ dsp: comp({ threshold: -25, ratio: 8, knee: 12 }) }) });
  const saved = (await rest('/api/buses')).body.find(x => x.id === bus);
  ok('PATCH persists the compressor settings',
     saved.dsp.comp.on === true && saved.dsp.comp.threshold === -25 &&
     saved.dsp.comp.ratio === 8 && saved.dsp.comp.knee === 12,
     JSON.stringify(saved.dsp.comp));

  const doc = (await rest('/api/project')).body;
  await rest('/api/project/document', {
    method: 'PUT',
    body: JSON.stringify({ name: doc.name, items: doc.items,
                           cartItems: doc.cartItems ?? [],
                           settings: doc.settings, theme: doc.theme }) });
  const after = (await rest('/api/buses')).body.find(x => x.id === bus);
  ok('the compressor survives a document round trip',
     after && after.dsp.comp.threshold === -25 && after.dsp.comp.ratio === 8,
     after ? JSON.stringify(after.dsp.comp) : '(bus missing)');

  await rest(`/api/project/items/${uuid}/stop`, { method: 'POST', body: '{}' });
  ws.close();
  console.log(`\n${failures === 0 ? 'ALL PASS' : 'FAILURES'} (${failures})`);
  process.exit(failures === 0 ? 0 : 1);
})().catch(e => { console.error('harness error:', e); process.exit(2); });

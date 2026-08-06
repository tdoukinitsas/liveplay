// Do the stereo image controls work, end to end?
//
// Two controls with two different mechanisms behind them, which is the reason
// this exists as its own harness:
//
//   WIDTH is a per-sample M/S matrix inside the strip's DSP chain, so it shows
//   up on the BUS meter. It has to be DSP rather than send gains because the
//   matrix needs a negative cross-term above unity and send gains are decibels.
//
//   BALANCE is nothing but the two send gains, so it shows up on the MASTER
//   meter and not on the bus meter at all. Measuring it in the wrong place
//   would read as balance doing nothing.
//
// Levels are checked against arithmetic, not against "did it move": the signal
// has equal, uncorrelated mid and side content, so the powers add and every
// width setting has one right answer. See gen-wide-signal.js.
//
// usage: node width-e2e.js <port> <wideWavPath>
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

// The width block ramps over ~20 ms and the bus meter integrates on top of
// that; the correlation meter smooths over roughly 100 ms as well. This is
// generous for all three.
const SETTLE_MS = 1500;

class Meters {
  constructor(ws) { this.f = []; ws.on('message', raw => {
    let m; try { m = JSON.parse(raw); } catch { return; }
    if (m.type === 'meters') this.f.push(m); }); }
  reset() { this.f = []; }
  // Average POWER, converted once at the end. A mean of decibel readings is a
  // geometric mean and weights quiet frames far too heavily.
  rms(id) {
    let sum = 0, n = 0;
    for (const fr of this.f) for (const c of (fr.mixer_channels || []))
      if (c.mixer_id === id && (c.rms_db ?? -200) > -190) { sum += Math.pow(10, c.rms_db / 10); n++; }
    return n ? 10 * Math.log10(sum / n) : -200;
  }
  correlation(id) {
    let sum = 0, n = 0;
    for (const fr of this.f) for (const c of (fr.mixer_channels || []))
      if (c.mixer_id === id && typeof c.correlation === 'number') { sum += c.correlation; n++; }
    return n ? sum / n : NaN;
  }
  // peak_max_db, not peak_db: the raw sample maximum since the last read,
  // with no ballistics, so it is the same quantity on the bus and on the
  // master and the two can be compared directly.
  busPeak(id) {
    let best = -200;
    for (const fr of this.f) for (const c of (fr.mixer_channels || []))
      if (c.mixer_id === id) best = Math.max(best, c.peak_max_db ?? -200);
    return best;
  }
  masterPeak(index) {
    let best = -200;
    for (const fr of this.f) for (const c of (fr.master_channels || []))
      if (c.index === index) best = Math.max(best, c.peak_db ?? -200);
    return best;
  }
}
const measure = async (m, ms) => { m.reset(); await sleep(ms); };

const uuid = 'item-width-0001';
const width = (o) => ({ width: { width: 1, bassMonoHz: 20, bassMonoQ: 0.7071, ...o } });

(async () => {
  await rest('/api/project/document', {
    method: 'PUT',
    body: JSON.stringify({ name: 'width', items: [{
      uuid, type: 'audio', displayName: 'Wide', mediaServerPath: WAV,
      volume: 1, endBehavior: 'stop' }] }),
  });
  for (let i = 0; i < 80; i++) {
    const p = await rest('/api/project/progress');
    if (p.body && p.body.loading === false) break;
    await sleep(100);
  }

  const bus = (await rest('/api/buses', {
    method: 'POST', body: JSON.stringify({ name: 'Wide', width: 2 }) })).body.id;
  await rest(`/api/project/items/${uuid}`, {
    method: 'PATCH', body: JSON.stringify({ busId: bus }) });

  const b = (await rest('/api/buses')).body.find(x => x.id === bus);
  ok('the bus reports width settings', !!b.dsp.width, JSON.stringify(b.dsp.width));
  ok('width starts at unity with bass-mono parked',
     b.dsp.width.width === 1 && b.dsp.width.bassMonoHz === 20);

  const ws = new WebSocket(`ws://127.0.0.1:${PORT}/ws`);
  await new Promise((res, rej) => { ws.on('open', res); ws.on('error', rej); });
  const m = new Meters(ws);

  await rest(`/api/project/items/${uuid}/play`, { method: 'POST', body: '{}' });
  await sleep(SETTLE_MS);
  await measure(m, 2000);
  const unity = m.rms(b.mixerId);
  ok('signal is reaching the bus', unity > -40, `${unity.toFixed(1)} dB RMS`);
  ok('equal mid and side reads as correlation 0',
     Math.abs(m.correlation(b.mixerId)) < 0.15, m.correlation(b.mixerId).toFixed(3));

  // ---- Width 0: mono. Side gone, so half the power. ----
  await rest(`/api/buses/${bus}/dsp`, {
    method: 'POST', body: JSON.stringify(width({ width: 0 })) });
  await sleep(SETTLE_MS);
  await measure(m, 2000);
  ok('width 0 drops the side content: -3.01 dB',
     Math.abs((m.rms(b.mixerId) - unity) + 3.01) < 0.5,
     `${unity.toFixed(1)} -> ${m.rms(b.mixerId).toFixed(1)} dB RMS`);
  ok('and correlation goes to +1', m.correlation(b.mixerId) > 0.95,
     m.correlation(b.mixerId).toFixed(3));

  // ---- Width 2: side power x4, so mid + 4*side against mid + side. ----
  await rest(`/api/buses/${bus}/dsp`, {
    method: 'POST', body: JSON.stringify(width({ width: 2 })) });
  await sleep(SETTLE_MS);
  await measure(m, 2000);
  ok('width 2 quadruples the side power: +3.98 dB',
     Math.abs((m.rms(b.mixerId) - unity) - 3.98) < 0.5,
     `${unity.toFixed(1)} -> ${m.rms(b.mixerId).toFixed(1)} dB RMS`);
  // The warning the correlation readout exists to give: past unity the lanes
  // start fighting, and a mono sum would lose the side content entirely.
  ok('and correlation goes negative', m.correlation(b.mixerId) < -0.4,
     m.correlation(b.mixerId).toFixed(3));

  // ---- Bass mono: the side tone is at 60 Hz, so it should go. ----
  await rest(`/api/buses/${bus}/dsp`, {
    method: 'POST', body: JSON.stringify(width({ width: 1, bassMonoHz: 200 })) });
  await sleep(SETTLE_MS);
  await measure(m, 2000);
  ok('bass-mono removes a 60 Hz side signal: -3 dB',
     Math.abs((m.rms(b.mixerId) - unity) + 3.01) < 0.6,
     `${unity.toFixed(1)} -> ${m.rms(b.mixerId).toFixed(1)} dB RMS`);
  ok('and the strip becomes mono-compatible', m.correlation(b.mixerId) > 0.9,
     m.correlation(b.mixerId).toFixed(3));

  // ---- Back to unity: the block must leave no residue. ----
  await rest(`/api/buses/${bus}/dsp`, {
    method: 'POST', body: JSON.stringify(width({})) });
  await sleep(SETTLE_MS);
  await measure(m, 2000);
  ok('returning to unity restores the original level exactly',
     Math.abs(m.rms(b.mixerId) - unity) < 0.2,
     `${unity.toFixed(1)} -> ${m.rms(b.mixerId).toFixed(1)} dB RMS`);

  // ---- Balance is a SEND gain, so it is measured on the master ----
  await measure(m, 2000);
  const houseL = m.masterPeak(0), houseR = m.masterPeak(1);
  const busPk  = m.busPeak(b.mixerId);
  ok('the bus reaches both sides of the house at centre',
     houseL > -40 && Math.abs(houseL - houseR) < 1.0,
     `L ${houseL.toFixed(1)} / R ${houseR.toFixed(1)} dB`);
  // The absolute check, and the one that pins the LAW rather than just its
  // symmetry: centre balance is unity, so the house sees exactly what the bus
  // sent. Every assertion below is relative to houseL, so without this one a
  // pan law applied to a stereo bus would shift the whole set 3 dB down
  // together and only the "does not add gain" check would notice.
  ok('and does so at unity — centre balance is not -3 dB',
     Math.abs(houseL - busPk) < 0.5,
     `bus ${busPk.toFixed(1)} -> house ${houseL.toFixed(1)} dB`);

  await rest(`/api/buses/${bus}/pan`, {
    method: 'POST', body: JSON.stringify({ pan: 1 }) });
  await sleep(SETTLE_MS);
  await measure(m, 2000);
  const hardL = m.masterPeak(0), hardR = m.masterPeak(1);
  ok('balance hard right silences the left side of the house',
     hardL < houseL - 20, `${houseL.toFixed(1)} -> ${hardL.toFixed(1)} dB`);
  // The whole point of the balance law: it only ever takes away, so the side
  // being moved toward must not get louder. The pan law would have added 3 dB.
  ok('and does NOT add gain to the right', Math.abs(hardR - houseR) < 0.6,
     `${houseR.toFixed(1)} -> ${hardR.toFixed(1)} dB`);

  await rest(`/api/buses/${bus}/pan`, {
    method: 'POST', body: JSON.stringify({ pan: 0 }) });
  await sleep(SETTLE_MS);
  await measure(m, 2000);
  ok('and centre puts it back at unity, not -3 dB',
     Math.abs(m.masterPeak(0) - houseL) < 0.5,
     `${houseL.toFixed(1)} -> ${m.masterPeak(0).toFixed(1)} dB`);

  // ---- A mono bus has no image, and width must not invent one ----
  const mono = (await rest('/api/buses', {
    method: 'POST', body: JSON.stringify({ name: 'Narrow', width: 1 }) })).body.id;
  await rest(`/api/project/items/${uuid}`, {
    method: 'PATCH', body: JSON.stringify({ busId: mono }) });
  const mb = (await rest('/api/buses')).body.find(x => x.id === mono);
  await sleep(SETTLE_MS);
  await measure(m, 2000);
  const monoBase = m.rms(mb.mixerId);
  await rest(`/api/buses/${mono}/dsp`, {
    method: 'POST', body: JSON.stringify(width({ width: 2 })) });
  await sleep(SETTLE_MS);
  await measure(m, 2000);
  ok('width does nothing to a mono bus',
     Math.abs(m.rms(mb.mixerId) - monoBase) < 0.3,
     `${monoBase.toFixed(1)} -> ${m.rms(mb.mixerId).toFixed(1)} dB RMS`);
  ok('and a mono bus reports correlation +1', m.correlation(mb.mixerId) > 0.99,
     m.correlation(mb.mixerId).toFixed(3));

  // ---- The mono-sum audition ----
  // PFL the wide bus into Monitor, then fold Monitor to mono. The check is on
  // the MONITOR strip: the bus being auditioned must not move at all, because
  // the whole promise is that this touches the phones and nothing else.
  //
  // Monitor is unbound on a machine with no headphone output, but its strip
  // still runs and still meters, so this measures correctly either way.
  await rest(`/api/project/items/${uuid}`, {
    method: 'PATCH', body: JSON.stringify({ busId: bus }) });
  await rest(`/api/buses/${bus}/pfl`, {
    method: 'POST', body: JSON.stringify({ pfl: true }) });
  await sleep(SETTLE_MS);
  await measure(m, 2000);
  const monitorBus = (await rest('/api/buses')).body.find(x => x.id === 'monitor');
  const monWide = m.rms(monitorBus.mixerId);
  const busWide = m.rms(b.mixerId);
  ok('PFL puts the wide bus into Monitor', monWide > -40, `${monWide.toFixed(1)} dB RMS`);
  ok('and Monitor starts un-folded', monitorBus.monoCheck === false);

  await rest('/api/monitor/mono', {
    method: 'POST', body: JSON.stringify({ mono: true }) });
  await sleep(SETTLE_MS);
  await measure(m, 2000);
  // The side content is uncorrelated with the mid, so folding drops it: the
  // monitor loses 3 dB and becomes mono-compatible.
  ok('MONO folds the monitor: -3.01 dB',
     Math.abs((m.rms(monitorBus.mixerId) - monWide) + 3.01) < 0.6,
     `${monWide.toFixed(1)} -> ${m.rms(monitorBus.mixerId).toFixed(1)} dB RMS`);
  ok('and the monitor reads as mono-compatible',
     m.correlation(monitorBus.mixerId) > 0.95,
     m.correlation(monitorBus.mixerId).toFixed(3));
  // The safety claim, and the reason this is on the Monitor strip rather than
  // on each channel: the bus being checked is untouched.
  ok('while the bus being auditioned does not move',
     Math.abs(m.rms(b.mixerId) - busWide) < 0.2,
     `${busWide.toFixed(1)} -> ${m.rms(b.mixerId).toFixed(1)} dB RMS`);
  ok('and the API reports the fold',
     (await rest('/api/buses')).body.find(x => x.id === 'monitor').monoCheck === true);

  await rest('/api/monitor/mono', {
    method: 'POST', body: JSON.stringify({ mono: false }) });
  await sleep(SETTLE_MS);
  await measure(m, 2000);
  ok('and releasing it restores the monitor exactly',
     Math.abs(m.rms(monitorBus.mixerId) - monWide) < 0.3,
     `${monWide.toFixed(1)} -> ${m.rms(monitorBus.mixerId).toFixed(1)} dB RMS`);
  await rest(`/api/buses/${bus}/pfl`, {
    method: 'POST', body: JSON.stringify({ pfl: false }) });

  // ---- Persistence ----
  await rest(`/api/buses/${bus}`, {
    method: 'PATCH',
    body: JSON.stringify({ dsp: width({ width: 1.6, bassMonoHz: 140 }), pan: -0.4 }) });
  const saved = (await rest('/api/buses')).body.find(x => x.id === bus);
  ok('PATCH persists width and balance',
     Math.abs(saved.dsp.width.width - 1.6) < 1e-4 &&
     Math.abs(saved.dsp.width.bassMonoHz - 140) < 1e-4 &&
     Math.abs(saved.pan + 0.4) < 1e-4,
     JSON.stringify({ width: saved.dsp.width, pan: saved.pan }));

  const doc = (await rest('/api/project')).body;
  await rest('/api/project/document', {
    method: 'PUT',
    body: JSON.stringify({ name: doc.name, items: doc.items,
                           cartItems: doc.cartItems ?? [],
                           settings: doc.settings, theme: doc.theme }) });
  const after = (await rest('/api/buses')).body.find(x => x.id === bus);
  ok('they survive a document round trip',
     after && Math.abs(after.dsp.width.width - 1.6) < 1e-4 &&
     Math.abs(after.pan + 0.4) < 1e-4,
     after ? JSON.stringify({ width: after.dsp.width, pan: after.pan }) : '(bus missing)');

  await rest(`/api/project/items/${uuid}/stop`, { method: 'POST', body: '{}' });
  ws.close();
  console.log(`\n${failures === 0 ? 'ALL PASS' : 'FAILURES'} (${failures})`);
  process.exit(failures === 0 ? 0 : 1);
})().catch(e => { console.error('harness error:', e); process.exit(2); });

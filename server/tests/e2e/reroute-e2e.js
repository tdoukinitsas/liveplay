// Does re-assigning a bus move audio that is ALREADY PLAYING?
//
// Routing used to be established only in play_item(), so the mixer said one
// thing and the audio did another until the cue was fired again. Asserted on
// meters, because that is the claim.
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

class Meters {
  constructor(ws) { this.f = []; ws.on('message', raw => {
    let m; try { m = JSON.parse(raw); } catch { return; }
    if (m.type === 'meters') this.f.push(m); }); }
  reset() { this.f = []; }
  peak(id) { let b = -200; for (const fr of this.f) for (const c of (fr.mixer_channels || []))
    if (c.mixer_id === id) b = Math.max(b, c.peak_db ?? -200); return b; }
}
const measure = async (m, ms) => { m.reset(); await sleep(ms); };

const uuid = 'item-reroute-0001';

(async () => {
  await rest('/api/project/document', {
    method: 'PUT',
    body: JSON.stringify({ name: 'reroute', items: [{
      uuid, type: 'audio', displayName: 'Tone', mediaServerPath: WAV,
      volume: 1, endBehavior: 'stop' }] }),
  });
  for (let i = 0; i < 80; i++) {
    const p = await rest('/api/project/progress');
    if (p.body && p.body.loading === false) break;
    await sleep(100);
  }

  const busA = (await rest('/api/buses', { method: 'POST', body: JSON.stringify({ name: 'RouteA', width: 2 }) })).body.id;
  const busB = (await rest('/api/buses', { method: 'POST', body: JSON.stringify({ name: 'RouteB', width: 2 }) })).body.id;
  await rest(`/api/project/items/${uuid}`, { method: 'PATCH', body: JSON.stringify({ busId: busA }) });

  let buses = (await rest('/api/buses')).body;
  const mixA = buses.find(b => b.id === busA).mixerId;
  const mixB = buses.find(b => b.id === busB).mixerId;
  ok('both test buses have strips', !!mixA && !!mixB);

  const ws = new WebSocket(`ws://127.0.0.1:${PORT}/ws`);
  await new Promise((res, rej) => { ws.on('open', res); ws.on('error', rej); });
  const m = new Meters(ws);

  // Start it playing on A, and leave it playing for the whole test.
  await rest(`/api/project/items/${uuid}/play`, { method: 'POST', body: '{}' });
  await sleep(600);
  await measure(m, 800);
  const aPlaying = m.peak(mixA), bIdle = m.peak(mixB);
  ok('audio is on bus A', aPlaying > -30, `A ${aPlaying.toFixed(1)} dBFS`);
  ok('bus B is silent',   bIdle < -60,    `B ${bIdle.toFixed(1)} dBFS`);

  // Strip meters fall back at roughly 4 dB/s, so a bus that has just been
  // vacated reads as a descending number for seconds. The claim is that the
  // level *moved*, so assert the drop rather than an absolute floor the
  // ballistics will not reach inside the window.
  const DROP = 12;
  const SETTLE = 1500;

  // Re-assign WITHOUT stopping or replaying. This is the whole test.
  await rest(`/api/project/items/${uuid}`, { method: 'PATCH', body: JSON.stringify({ busId: busB }) });
  await sleep(SETTLE);
  await measure(m, 800);
  const aAfter = m.peak(mixA), bAfter = m.peak(mixB);
  ok('audio moved to bus B without replaying', bAfter > -30, `B ${bAfter.toFixed(1)} dBFS`);
  ok('bus A gave it up', aAfter < aPlaying - DROP, `A ${aPlaying.toFixed(1)} -> ${aAfter.toFixed(1)} dBFS`);

  // And back again, to be sure it is not a one-way latch.
  await rest(`/api/project/items/${uuid}`, { method: 'PATCH', body: JSON.stringify({ busId: busA }) });
  await sleep(SETTLE);
  await measure(m, 800);
  const aBack = m.peak(mixA), bBack = m.peak(mixB);
  ok('and back to bus A', aBack > -30 && bBack < bAfter - DROP,
     `A ${aBack.toFixed(1)} / B ${bAfter.toFixed(1)} -> ${bBack.toFixed(1)} dBFS`);

  await rest(`/api/project/items/${uuid}/stop`, { method: 'POST', body: '{}' });
  ws.close();
  console.log(`\n${failures === 0 ? 'ALL PASS' : 'FAILURES'} (${failures})`);
  process.exit(failures === 0 ? 0 : 1);
})().catch(e => { console.error('harness error:', e); process.exit(2); });

// A bus whose output names hardware this machine does not have must go SILENT,
// not to the default device.
//
// open_device_by_name() falls back to the default device when a name matches
// nothing (engine.cpp), and OutputMap's identity fallback used to hand it any
// unmapped name. So a project whose sub-mix targeted "Scarlett 2i2", opened at
// a venue without one, did not go quiet — it arrived in the house. That is the
// accident PFL was chosen over solo to make impossible, reached through
// routing instead.
//
// The preview bus has always had the strict rule. This pins it for every
// hardware output: a real mapping wins, an unmapped name is a device name only
// if that device is present, and otherwise the bus is silent and reports
// `bound: false` so the surface can say so and the operator can decide.
//
// Measured on the meters, not read back from JSON: "is it bound" is a claim
// about a flag, and the claim that matters is about where the audio went.
//
// usage: node absent-device-e2e.js <port> <wavPath>
const WebSocket = require(require.resolve('ws', { paths: [process.cwd()] }));

const PORT = process.argv[2] || '4480';
const WAV  = process.argv[3];
const BASE = `http://127.0.0.1:${PORT}`;

let failures = 0;
const ok = (name, pass, detail) => {
  if (detail === undefined) detail = '(no detail given — fix the assertion)';
  console.log(`${pass ? 'PASS' : 'FAIL'}  ${name}   ${detail}`);
  if (!pass) failures++;
};
const dB = x => (typeof x === 'number' ? x.toFixed(1) : String(x));
const sleep = ms => new Promise(r => setTimeout(r, ms));

async function rest(path, opts = {}) {
  const r = await fetch(BASE + path, { headers: { 'content-type': 'application/json' }, ...opts });
  const text = await r.text();
  let body; try { body = JSON.parse(text); } catch { body = text; }
  return { status: r.status, body };
}
const buses = async () => (await rest('/api/buses')).body;
const bus   = async id => (await buses()).find(b => b.id === id);
const patch = (id, body) => rest(`/api/buses/${id}`, { method: 'PATCH', body: JSON.stringify(body) });
const play  = uuid => rest(`/api/project/items/${uuid}/play`, { method: 'POST', body: '{}' });
const stop  = uuid => rest(`/api/project/items/${uuid}/stop`, { method: 'POST', body: '{}' });

const GHOST = 'No Such Interface 9f3c2a';

class Meters {
  constructor(ws) {
    this.frames = [];
    ws.on('message', raw => {
      let m; try { m = JSON.parse(raw); } catch { return; }
      if (m.type === 'meters') this.frames.push(m);
    });
  }
  reset() { this.frames = []; }
  masterPeak(pred) {
    let best = -200;
    for (const f of this.frames)
      for (const c of (f.master_channels || []))
        if (pred(c.index)) best = Math.max(best, c.peak_db ?? -200);
    return best;
  }
  // The house is masters 0/1 ONLY. The reserved preview pair is 30/31 at the
  // default width; everything between is the pool. Maxing over every master
  // silently stops meaning "the house" the moment preview is bound.
  house()    { return this.masterPeak(i => i <= 1); }
  reserved() { return this.masterPeak(i => i >= 30); }
  pool()     { return this.masterPeak(i => i > 1 && i < 30); }
  anyMaster() { return this.masterPeak(() => true); }
}
async function measure(m, ms) { m.reset(); await sleep(ms); }
async function waitLoaded() {
  for (let i = 0; i < 80; i++) {
    const p = await rest('/api/project/progress');
    if (p.body && p.body.loading === false) return;
    await sleep(100);
  }
}
// Master meters fall back at roughly 4 dB/s, so a baseline has to be waited
// for rather than assumed — especially running back to back with other suites.
async function waitHouseSilent(m, floor = -60, maxMs = 20000) {
  let v = -200;
  for (let t = 0; t < maxMs; t += 500) {
    await measure(m, 500);
    v = m.anyMaster();
    if (v < floor) break;
  }
  return v;
}

(async () => {
  const ws = new WebSocket(`ws://127.0.0.1:${PORT}/ws`);
  await new Promise((res, rej) => { ws.on('open', res); ws.on('error', rej); });
  const m = new Meters(ws);

  // A bus pointed at hardware that is not here, with a cue on it.
  await rest('/api/project/document', {
    method: 'PUT',
    body: JSON.stringify({
      name: 'absent-device-e2e',
      items: [
        { uuid: 'item-g', type: 'audio', displayName: 'OnGhost',
          mediaServerPath: WAV, volume: 1, endBehavior: 'loop', busId: 'ghost' },
      ],
      // Master and preview are declared explicitly. With only one bus in the
      // document the role migration promotes it to master (D35), which then
      // holds the house pair by role whatever its target resolves to — a
      // different and much weaker test than the ordinary sub-mix meant here.
      buses: [
        { id: 'master',  name: 'Master',  width: 2, gainDb: 0, master: true,
          output: { type: 'output', target: 'Main Out' } },
        { id: 'preview', name: 'Preview', width: 2, gainDb: 0, preview: true,
          output: { type: 'output', target: 'Preview Out' } },
        { id: 'ghost',   name: 'Ghost',   width: 2, gainDb: 0,
          output: { type: 'output', target: GHOST } },
      ],
    }),
  });
  await waitLoaded();
  await rest('/api/buses/pfl/clear', { method: 'POST', body: '{}' });
  await rest('/api/preview', { method: 'DELETE' });
  await rest('/api/outputs', { method: 'PUT', body: JSON.stringify({ version: 1, outputs: [] }) });

  let g = await bus('ghost');
  ok('a bus naming absent hardware is unbound',
     g?.bound === false, `bound ${g?.bound} target ${JSON.stringify(g?.output)}`);
  ok('and it holds no master pair',
     g?.masters === null || g?.masters === undefined, JSON.stringify(g?.masters));

  // ---- The claim: playing through it reaches nothing --------------------
  const base = await waitHouseSilent(m);
  ok('baseline: every master is quiet before the test signal',
     base < -60, `${dB(base)} dBFS`);

  await play('item-g');
  await measure(m, 2500);
  const houseGhost = m.house();
  const poolGhost  = m.pool();
  const resGhost   = m.reserved();
  // THIS is the load-bearing one. An ordinary Output-kind bus is allocated a
  // pool pair, so the old default-device fallback surfaced HERE, not on the
  // house pair — the audio still physically left the default sound card, which
  // in a venue is the house, but masters 0/1 stayed quiet throughout. Against
  // the unfixed build this reads about -6 dBFS on a pool pair.
  ok('playing through it reaches NO hardware at all',
     poolGhost < -60 && resGhost < -60, `pool ${dB(poolGhost)} reserved ${dB(resGhost)}`);
  // Worth pinning even though it holds either way, because it is the claim
  // someone will assume this suite is making: whatever else happens, a
  // sub-mix must never appear on the house pair.
  ok('and the house pair is untouched (true before the fix too)',
     houseGhost < -60, `house ${dB(houseGhost)} dBFS`);
  await stop('item-g');

  // ---- The contrast, so the silence above means something ---------------
  // Without this, "the house is quiet" would also pass with a dead harness, a
  // wav that never loaded, or a cue that never played. Same bus, same cue,
  // pointed at a target that DOES resolve.
  await waitHouseSilent(m);
  await patch('ghost', { output: { type: 'output', target: 'Main Out' } });
  g = await bus('ghost');
  ok('re-pointed at Main Out the same bus becomes bound',
     g?.bound === true, `bound ${g?.bound}`);

  await play('item-g');
  await measure(m, 2500);
  const poolMain = m.pool();
  // A POOL pair, not the house: an ordinary Output-kind bus is allocated a
  // pair from the pool (≥2), while masters 0/1 belong to the master role
  // (D27). Asserting on the house here would fail for a reason that has
  // nothing to do with what this suite is about.
  ok('and the same cue on the same bus now DOES reach hardware',
     poolMain > -40, `pool ${dB(poolMain)} dBFS (house ${dB(m.house())})`);
  await stop('item-g');

  // ---- The preview bus keeps the stricter rule --------------------------
  // Main Out unmapped means the default device, and the default device is the
  // house. The preview bus must not get that branch even though every other
  // bus does, or PFL lands in the house.
  await waitHouseSilent(m);
  const P = (await buses()).find(b => b.preview);
  await patch(P.id, { output: { type: 'output', target: 'Main Out' } });
  const P2 = (await buses()).find(b => b.preview);
  ok('the preview bus on Main Out is still unbound, not on the default device',
     P2?.bound === false, `bound ${P2?.bound} target ${JSON.stringify(P2?.output)}`);

  ws.close();
  console.log(`\n${failures === 0 ? 'ALL PASS' : 'FAILURES'} (${failures})`);
  process.exit(failures === 0 ? 0 : 1);
})().catch(e => { console.error('harness error:', e); process.exit(2); });

// C4: end-to-end check of the C1/C2 migration work against a live server.
//
// The claims (D11, D12, D17) are behavioural, so they are driven through the
// real REST/WS surface rather than by inspecting state:
//   1. A document that predates buses (no `buses`, no `busSchema`) takes the
//      full migration path: Main + Monitor are synthesised as system buses,
//      every item resolves to Main, and every connected client sees a
//      `project_migrated` broadcast.
//   2. A 2.4-style client document carrying two distinct per-item
//      `deviceOverride`s gets two Output-kind buses synthesised, one per
//      device; the items are assigned to them; the overrides are erased from
//      the document (not just the response); and audio from an override item
//      is actually heard on the bus it was assigned to.
//   3. Re-PUTting that same, now-migrated document reports a zero-count
//      migration and broadcasts nothing — migration is a one-time event, not
//      a property of the document shape.
//   4. A bus-era document that omits `buses` but declares `busSchema` is a
//      save-shaped round trip: the previously-loaded buses carry across
//      untouched (D11's other half — the version is the signal, not the
//      presence/absence of the key on its own).
//
// usage: node migration-e2e.js <port> <wavPath>
const WebSocket = require(require.resolve('ws', { paths: [process.cwd()] }));

const PORT = process.argv[2] || '4630';
const WAV  = process.argv[3];
const BASE = `http://127.0.0.1:${PORT}`;

let failures = 0;
const ok = (name, pass, detail = '') => {
  console.log(`${pass ? 'PASS' : 'FAIL'}  ${name}${detail ? '   ' + detail : ''}`);
  if (!pass) failures++;
};

async function rest(path, opts = {}) {
  const r = await fetch(BASE + path, {
    headers: { 'content-type': 'application/json' },
    ...opts,
  });
  const text = await r.text();
  let body; try { body = JSON.parse(text); } catch { body = text; }
  return { status: r.status, body };
}
const sleep = ms => new Promise(r => setTimeout(r, ms));
const buses = async () => (await rest('/api/buses')).body;
const doc   = async () => (await rest('/api/project')).body;
const settle = async () => {
  for (let i = 0; i < 100; i++) {
    const p = await rest('/api/project/progress');
    if (p.body && p.body.loading === false) return;
    await sleep(100);
  }
};

class Meters {
  constructor(ws) {
    this.frames = [];
    ws.on('message', raw => {
      let m; try { m = JSON.parse(raw); } catch { return; }
      if (m.type === 'meters') this.frames.push(m);
    });
  }
  reset() { this.frames = []; }
  peak(mixerId) {
    let best = -200;
    for (const f of this.frames)
      for (const c of (f.mixer_channels || []))
        if (c.mixer_id === mixerId) best = Math.max(best, c.peak_db ?? -200);
    return best;
  }
}
async function measure(m, ms) { m.reset(); await sleep(ms); }

// A frame recorder for doc_patch traffic. Two independent WS clients are
// used throughout: `frames` belongs to the client that also drives REST
// calls, `frames2` belongs to a second, otherwise-idle connection whose only
// job is to prove the broadcast reaches someone who didn't ask for it (D17).
function patchRecorder(ws) {
  let frames = [];
  ws.on('message', raw => {
    let j; try { j = JSON.parse(raw); } catch { return; }
    if (j.type === 'doc_patch') frames.push(j);
  });
  return { drain: () => { const f = frames; frames = []; return f; } };
}

(async () => {
  const ws1 = new WebSocket(`ws://127.0.0.1:${PORT}/ws`);
  const ws2 = new WebSocket(`ws://127.0.0.1:${PORT}/ws`);
  await Promise.all([
    new Promise((res, rej) => { ws1.on('open', res); ws1.on('error', rej); }),
    new Promise((res, rej) => { ws2.on('open', res); ws2.on('error', rej); }),
  ]);
  const rec1 = patchRecorder(ws1);
  const rec2 = patchRecorder(ws2);
  const migratedFrames = frames => frames.filter(f => f.op === 'project_migrated');

  // ===== 1. A document that predates buses: full migration, no carry ======
  let r = await rest('/api/project/document', {
    method: 'PUT',
    body: JSON.stringify({
      name: 'legacy-plain',
      items: [
        { uuid: 'p1', type: 'audio', displayName: 'One' },
        { uuid: 'p2', type: 'audio', displayName: 'Two' },
      ],
    }),
  });
  ok('1: doc with no buses/busSchema is accepted', r.status === 200, `status ${r.status}`);
  await settle();
  await sleep(300);

  ok('1: response carries a migration summary', !!r.body.migration,
     JSON.stringify(r.body.migration || null));
  ok('1: itemsToMain counts both items',
     r.body.migration && r.body.migration.itemsToMain === 2,
     r.body.migration && String(r.body.migration.itemsToMain));

  let bs = await buses();
  const main1 = bs.find(b => b.id === 'main'), mon1 = bs.find(b => b.id === 'monitor');
  ok('1: Main bus exists', !!main1);
  ok('1: Monitor bus exists', !!mon1);
  ok('1: Main is a system bus', !!main1 && main1.system === true);
  ok('1: Monitor is a system bus', !!mon1 && mon1.system === true);

  let d1 = await doc();
  ok('1: both items resolve to Main',
     d1.items.every(i => !i.busId || i.busId === 'main'),
     JSON.stringify(d1.items.map(i => i.busId)));

  const f1 = migratedFrames(rec1.drain());
  const f1b = migratedFrames(rec2.drain());
  ok('1: project_migrated observed on the driving client', f1.length === 1, `${f1.length} frame(s)`);
  ok('1: project_migrated observed on a second, idle client (D17)', f1b.length === 1, `${f1b.length} frame(s)`);
  ok('1: broadcast frame carries flat counts, not nested',
     f1[0] && typeof f1[0].itemsToMain === 'number' && !('migration' in (f1[0] || {})),
     JSON.stringify(f1[0] || null));

  // ===== 2. A 2.4-style doc with two distinct deviceOverrides ==============
  const devs = (await rest('/api/devices')).body;
  ok('2: at least one playback device is enumerated', Array.isArray(devs) && devs.length > 0,
     `${Array.isArray(devs) ? devs.length : 0} device(s)`);
  const devA = devs[0].display_name || devs[0].name;
  // A second, DISTINCT override string. Real hardware providing a second
  // device is not guaranteed in this environment, so the distinctness the
  // assertion cares about (two separate buses, two separate assignments, the
  // field erased) is proven with a synthetic second name; the audio claim is
  // then checked against the item whose override names a real device.
  const devB = devs.length > 1 ? (devs[1].display_name || devs[1].name) : (devA + ' (secondary)');

  r = await rest('/api/project/document', {
    method: 'PUT',
    body: JSON.stringify({
      name: 'legacy-override',
      items: [
        { uuid: 'ov1', type: 'audio', displayName: 'OverA', mediaServerPath: WAV,
          volume: 1, endBehavior: 'stop', deviceOverride: devA },
        { uuid: 'ov2', type: 'audio', displayName: 'OverB', deviceOverride: devB },
        { uuid: 'ov3', type: 'audio', displayName: 'Plain' },
      ],
    }),
  });
  ok('2: doc with deviceOverrides is accepted', r.status === 200, `status ${r.status}`);
  await settle();
  await sleep(400);

  console.log('   response.migration =', JSON.stringify(r.body.migration));
  ok('2: response carries a migration summary', !!r.body.migration);
  ok('2: busesFromDeviceOverride counts both distinct devices',
     r.body.migration && r.body.migration.busesFromDeviceOverride === 2,
     r.body.migration && String(r.body.migration.busesFromDeviceOverride));
  ok('2: itemsToMain counts only the plain item',
     r.body.migration && r.body.migration.itemsToMain === 1,
     r.body.migration && String(r.body.migration.itemsToMain));

  bs = await buses();
  const outputBuses = bs.filter(b => !b.system && b.output && b.output.type === 'output');
  ok('2: two Output-kind buses were synthesised', outputBuses.length === 2,
     JSON.stringify(outputBuses.map(b => ({ id: b.id, target: b.output.target }))));
  const busA = outputBuses.find(b => b.output.target === devA);
  const busB = outputBuses.find(b => b.output.target === devB);
  ok('2: one Output bus targets device A', !!busA, devA);
  ok('2: the other Output bus targets device B', !!busB, devB);

  let d2 = await doc();
  const itemA = d2.items.find(i => i.uuid === 'ov1');
  const itemB = d2.items.find(i => i.uuid === 'ov2');
  ok('2: item A was assigned busId A', !!busA && itemA.busId === busA.id, itemA.busId);
  ok('2: item B was assigned busId B', !!busB && itemB.busId === busB.id, itemB.busId);
  ok('2: deviceOverride is erased from the document, not just the response',
     !d2.items.some(i => 'deviceOverride' in i),
     JSON.stringify(d2.items.map(i => Object.keys(i))));

  const f2 = migratedFrames(rec1.drain());
  const f2b = migratedFrames(rec2.drain());
  ok('2: project_migrated observed once', f2.length === 1, `${f2.length} frame(s)`);
  ok('2: project_migrated observed once on a second client too (D17)', f2b.length === 1, `${f2b.length} frame(s)`);

  // The audio claim: play the item whose override names a real device and
  // confirm the bus it was assigned to actually carries the signal.
  const m = new Meters(ws1);
  await rest(`/api/project/items/ov1/play`, { method: 'POST', body: '{}' });
  await sleep(500);
  await measure(m, 900);
  const busAPeak = m.peak(busA.mixerId);
  ok('2: audio from an override item plays out the bus it was mapped to',
     busAPeak > -30, `bus '${busA.id}' ${busAPeak.toFixed(1)} dBFS`);
  await rest(`/api/project/items/ov1/stop`, { method: 'POST', body: '{}' });
  await sleep(200);

  // ===== 3. Idempotence: re-PUT the already-migrated document =============
  const again = JSON.parse(JSON.stringify(d2));
  delete again.itemCount;
  r = await rest('/api/project/document', { method: 'PUT', body: JSON.stringify(again) });
  await settle();
  await sleep(400);
  ok('3: second load of a migrated doc reports no migration',
     !r.body.migration, JSON.stringify(r.body.migration || null));
  const f3 = migratedFrames(rec1.drain());
  const f3b = migratedFrames(rec2.drain());
  ok('3: second load broadcasts no project_migrated (driving client)', f3.length === 0, `${f3.length} frame(s)`);
  ok('3: second load broadcasts no project_migrated (second client)', f3b.length === 0, `${f3b.length} frame(s)`);

  // ===== 4. A bus-era doc omitting `buses`, declaring `busSchema`: carry ===
  const stripsBefore = (await buses()).map(b => b.id + ':' + b.mixerId).sort().join('|');
  r = await rest('/api/project/document', {
    method: 'PUT',
    body: JSON.stringify({ name: 'legacy-override', busSchema: 1, items: again.items }),
  });
  ok('4: save-shaped round trip is accepted', r.status === 200, `status ${r.status}`);
  await settle();
  await sleep(300);
  const stripsAfter = (await buses()).map(b => b.id + ':' + b.mixerId).sort().join('|');
  ok('4: the prior buses are intact, same ids and strips',
     stripsBefore === stripsAfter, `${stripsBefore} -> ${stripsAfter}`);
  ok('4: no migration reported for a carried round trip',
     !r.body.migration, JSON.stringify(r.body.migration || null));
  const f4 = migratedFrames(rec1.drain());
  ok('4: no project_migrated broadcast for a carried round trip', f4.length === 0, `${f4.length} frame(s)`);

  ws1.close();
  ws2.close();
  console.log(`\n${failures === 0 ? 'ALL PASS' : 'FAILURES'} (${failures})`);
  process.exit(failures === 0 ? 0 : 1);
})().catch(e => { console.error('harness error:', e); process.exit(2); });

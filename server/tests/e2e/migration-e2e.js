// C4: end-to-end check of the C1/C2 migration work against a live server.
//
// The claims (D11, D12, D17) are behavioural, so they are driven through the
// real REST/WS surface rather than by inspecting state:
//   1. A document that predates buses (no `buses`, no `busSchema`) takes the
//      full migration path: the master-role and preview-role buses are
//      synthesised with the stock ids (D35), every item resolves to the
//      master bus, and every connected client sees a `project_migrated`
//      broadcast.
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
//   5. A round-1 document (`busSchema:1`, system Main/Monitor, a user bus of
//      the retired "master" output kind, `settings.previewDevice`) takes the
//      role migration (D25/D28/D34): Main becomes the master-role bus named
//      "Master" on "Main Out", the user bus becomes a bus->master send,
//      Monitor becomes the preview-role bus named "Preview" targeting the
//      device previewDevice named (and is bound), previewDevice is erased,
//      busSchema is written as 2, the summary and the broadcast say
//      rolesMigrated / previewDeviceMigrated, and a cue with no busId plays
//      out the HOUSE (masters 0/1). Re-PUTting the migrated document reports
//      nothing; a renamed Main keeps its name.
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
  const main1 = bs.find(b => b.master), mon1 = bs.find(b => b.preview);
  ok('1: a master-role bus exists, with the stock id', !!main1 && main1.id === 'master',
     JSON.stringify(bs.map(b => [b.id, b.master, b.preview])));
  ok('1: a preview-role bus exists, with the stock id', !!mon1 && mon1.id === 'preview',
     JSON.stringify(bs.map(b => [b.id, b.master, b.preview])));
  ok('1: exactly one of each role', bs.filter(b => b.master).length === 1 && bs.filter(b => b.preview).length === 1,
     JSON.stringify(bs.map(b => [b.id, b.master, b.preview])));
  ok('1: no bus reports the retired `system` flag', !bs.some(b => 'system' in b),
     JSON.stringify(bs.map(b => Object.keys(b).filter(k => k === 'system'))));
  ok('1: the master bus sends to Main Out and holds the house pair',
     !!main1 && main1.output.type === 'output' && main1.output.target === 'Main Out' &&
     JSON.stringify(main1.masters) === '[0,1]',
     main1 && `${JSON.stringify(main1.output)} masters ${JSON.stringify(main1.masters)}`);

  let d1 = await doc();
  ok('1: both items resolve to the master bus',
     d1.items.every(i => !i.busId || i.busId === 'master') &&
     bs.find(b => b.master).itemUuids.length === 2,
     JSON.stringify(d1.items.map(i => i.busId)) + ' / ' + JSON.stringify(bs.find(b => b.master).itemUuids));

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
  // The role holders are Output-kind buses too now (Main Out / Preview Out),
  // so the synthesised ones are the Output-kind buses WITHOUT a role.
  const outputBuses = bs.filter(b => !b.master && !b.preview && b.output && b.output.type === 'output');
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

  // ===== 5. A round-1 document: the role migration (D25/D28/D34) ==========
  // A non-default device where the machine has one, so that the migrated
  // target is provably not "whatever the default is" — falling back to the
  // default device is exactly what the preview path must never do.
  const previewDev = (devs.find(d => !d.is_default) || devs[0]);
  const previewDevName = previewDev.display_name || previewDev.name;
  console.log(`   previewDevice for 5 = "${previewDevName}"${previewDev.is_default ? ' (the default device; no other is present)' : ' (not the default device)'}`);

  await sleep(1200);   // the house meter falls away from section 2's cue
  rec1.drain(); rec2.drain();
  const v1 = {
    name: 'round-1-doc', busSchema: 1,
    settings: { previewDevice: previewDevName },
    buses: [
      { id: 'main',    name: 'Main',    width: 2, gainDb: 0, order: 1000000, system: true,
        output: { type: 'master', target: '' } },
      { id: 'monitor', name: 'Monitor', width: 2, gainDb: 0, order: 1000001, system: true,
        output: { type: 'output', target: 'Monitor' } },
      { id: 'b1',      name: 'Sub',     width: 2, gainDb: 0, order: 1,
        output: { type: 'master', target: '' } },
    ],
    items: [
      { uuid: 'v1-cue', type: 'audio', displayName: 'HouseCue', mediaServerPath: WAV,
        volume: 1, endBehavior: 'stop' },
    ],
  };
  r = await rest('/api/project/document', { method: 'PUT', body: JSON.stringify(v1) });
  ok('5: a busSchema:1 document is accepted', r.status === 200, `status ${r.status}`);
  await settle();
  await sleep(500);
  console.log('   response.migration =', JSON.stringify(r.body.migration));
  const mig5 = r.body.migration || {};
  ok('5: rolesMigrated is reported', mig5.rolesMigrated === true, JSON.stringify(mig5));
  ok('5: previewDeviceMigrated counts the one key', mig5.previewDeviceMigrated === 1, JSON.stringify(mig5));
  ok('5: nothing else was migrated (no overrides, no unassigned-item count for a bus-era doc)',
     mig5.busesFromDeviceOverride === 0 && mig5.itemsToMain === 0 && mig5.mainOutputMigrated === false,
     JSON.stringify(mig5));

  bs = await buses();
  const main5 = bs.find(b => b.id === 'main');
  const mon5  = bs.find(b => b.id === 'monitor');
  const sub5  = bs.find(b => b.id === 'b1');
  ok('5: "main" is the master-role bus, renamed Master, on Main Out',
     !!main5 && main5.master === true && main5.preview === false && main5.name === 'Master' &&
     main5.output.type === 'output' && main5.output.target === 'Main Out',
     JSON.stringify(main5 && { master: main5.master, name: main5.name, output: main5.output }));
  ok('5: the master bus is bound and holds the house pair',
     !!main5 && main5.bound === true && JSON.stringify(main5.masters) === '[0,1]',
     main5 && `bound ${main5.bound} masters ${JSON.stringify(main5.masters)}`);
  ok('5: the user bus of the retired "master" kind is now a bus->master send (D25)',
     !!sub5 && sub5.output.type === 'bus' && sub5.output.target === 'main' && !sub5.master && !sub5.preview,
     JSON.stringify(sub5 && sub5.output));
  ok('5: "monitor" is the preview-role bus, renamed Preview',
     !!mon5 && mon5.preview === true && mon5.master === false && mon5.name === 'Preview',
     JSON.stringify(mon5 && { preview: mon5.preview, name: mon5.name }));
  ok('5: settings.previewDevice migrated onto the preview bus as its target (D28)',
     !!mon5 && mon5.output.type === 'output' && mon5.output.target === previewDevName,
     JSON.stringify(mon5 && mon5.output));
  ok('5: ...which binds it (a present device name is bound, D26)',
     !!mon5 && mon5.bound === true, mon5 && `bound ${mon5.bound}`);
  ok('5: exactly one master and one preview after migration',
     bs.filter(b => b.master).length === 1 && bs.filter(b => b.preview).length === 1,
     JSON.stringify(bs.map(b => [b.id, b.master, b.preview])));

  let d5 = await doc();
  ok('5: settings.previewDevice is erased from the document',
     !d5.settings || !('previewDevice' in d5.settings),
     JSON.stringify(d5.settings ? Object.keys(d5.settings) : null));
  ok('5: the document is written as busSchema 2', d5.busSchema === 2, `busSchema ${d5.busSchema}`);
  ok('5: no bus in the document carries `system` or the retired output kind',
     Array.isArray(d5.buses) && !d5.buses.some(b => 'system' in b || (b.output && b.output.type === 'master')),
     JSON.stringify((d5.buses || []).map(b => ({ id: b.id, sys: 'system' in b, out: b.output }))));
  ok('5: the unassigned cue resolves to the master bus',
     main5 && main5.itemUuids.includes('v1-cue'), JSON.stringify(main5 && main5.itemUuids));

  const f5 = migratedFrames(rec1.drain());
  const f5b = migratedFrames(rec2.drain());
  ok('5: project_migrated observed once on the driving client', f5.length === 1, `${f5.length} frame(s)`);
  ok('5: project_migrated observed once on the idle client (D17)', f5b.length === 1, `${f5b.length} frame(s)`);
  ok('5: the broadcast carries rolesMigrated and previewDeviceMigrated',
     f5[0] && f5[0].rolesMigrated === true && f5[0].previewDeviceMigrated === 1 &&
     f5b[0] && f5b[0].rolesMigrated === true && f5b[0].previewDeviceMigrated === 1,
     JSON.stringify(f5[0] || null));

  // The audio claim: the cue plays out the HOUSE — masters 0/1, the pair the
  // master-role bus is wired on (D27). Before this round a migrated Main took
  // a pool pair and the house meters read nothing (§10.1 item 4).
  const houseOf = frames => {
    let best = -200;
    for (const f of frames)
      for (const c of (f.master_channels || []))
        if (c.index <= 1) best = Math.max(best, c.peak_db ?? -200);
    return best;
  };
  await rest('/api/project/items/v1-cue/play', { method: 'POST', body: '{}' });
  await sleep(500);
  await measure(m, 900);
  const house5 = houseOf(m.frames);
  const mainStrip5 = m.peak(main5.mixerId);
  ok('5: the cue plays out the house (masters 0/1 active)', house5 > -30, `${house5.toFixed(1)} dBFS`);
  ok('5: ...through the master bus\'s own strip', mainStrip5 > -30, `${mainStrip5.toFixed(1)} dBFS`);
  await rest('/api/project/items/v1-cue/stop', { method: 'POST', body: '{}' });
  await sleep(200);

  // Idempotence: the migrated document, as GET /api/project returns it,
  // PUT back: nothing to migrate, nothing broadcast.
  const again5 = JSON.parse(JSON.stringify(d5));
  delete again5.itemCount;
  r = await rest('/api/project/document', { method: 'PUT', body: JSON.stringify(again5) });
  await settle();
  await sleep(400);
  ok('5: a second PUT of the migrated document reports no migration',
     r.status === 200 && !r.body.migration, `status ${r.status} ${JSON.stringify(r.body.migration || null)}`);
  ok('5: ...and broadcasts no project_migrated',
     migratedFrames(rec1.drain()).length === 0 && migratedFrames(rec2.drain()).length === 0,
     '(frames drained)');
  bs = await buses();
  ok('5: the roles and names survive the round trip',
     bs.find(b => b.id === 'main').master && bs.find(b => b.id === 'main').name === 'Master' &&
     bs.find(b => b.id === 'monitor').preview && bs.find(b => b.id === 'monitor').name === 'Preview',
     JSON.stringify(bs.map(b => [b.id, b.name, b.master, b.preview])));

  // A renamed Main keeps its name: only the stock "Main" is restyled (D34).
  const v1named = JSON.parse(JSON.stringify(v1));
  v1named.name = 'round-1-renamed';
  v1named.buses[0].name = 'FOH';
  r = await rest('/api/project/document', { method: 'PUT', body: JSON.stringify(v1named) });
  await settle();
  await sleep(300);
  bs = await buses();
  const foh = bs.find(b => b.id === 'main');
  ok('5: a renamed Main keeps its name through the role migration',
     !!foh && foh.master === true && foh.name === 'FOH',
     JSON.stringify(foh && { name: foh.name, master: foh.master }));
  rec1.drain(); rec2.drain();

  ws1.close();
  ws2.close();
  console.log(`\n${failures === 0 ? 'ALL PASS' : 'FAILURES'} (${failures})`);
  process.exit(failures === 0 ? 0 : 1);
})().catch(e => { console.error('harness error:', e); process.exit(2); });

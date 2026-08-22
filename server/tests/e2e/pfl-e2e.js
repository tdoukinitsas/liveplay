// End-to-end check of PFL / pre-listen against a running liveplay-server.
//
// The claims are behavioural, so they are checked against real audio through
// the real render loop rather than by inspecting state:
//   1. A project with no buses gets a preview-role bus ("preview", "Preview")
//      on the built-in "Preview Out", which is silent until mapped (D26/D35).
//   2. Pointing the preview bus at another bus is refused, and changes nothing.
//   3. PFL cannot be raised on the preview bus itself.
//   4. PFL puts a bus into the preview strip; clearing takes it out.
//   5. The tap is PRE-FADER: fader at -inf, preview still hears it.
//   6. The tap is PRE-MUTE: bus muted, preview still hears it.
//   7. PFL never changes what the house hears.
//   8. A mono bus arrives centred, not hard left.
//   9. With no headphone output configured, the preview bus drives no hardware.
//  10. Mapped, it drives the reserved pair at the top of the master bus.
//  11. Cue pre-listen goes to that same bus — the preview bus IS pre-listen.
//  12. PFL and pre-listen sum there, which is the point of merging them.
//  13. A device NAME as the preview bus's target binds it with no output map
//      (D26/D29) — the path settings.previewDevice migrates onto (D28).
//
// "The house" means masters 0/1 specifically. Maxing over every master channel
// silently stopped meaning that once the preview bus was mapped, because the
// reserved pair is a master channel too — see Meters.housePeak.
//
// Role MOVES (PFL tap and reserved pair following the preview role, the
// house following the master role, the 409 matrix) live in roles-e2e.js.
//
// usage: node pfl-e2e.js <port> <wavPath>
// Resolved against the repo, not this scratch directory.
const WebSocket = require(require.resolve('ws', { paths: [process.cwd()] }));

const PORT = process.argv[2] || '4495';
const WAV  = process.argv[3];
const BASE = `http://127.0.0.1:${PORT}`;

let failures = 0;
// Every line prints the values it compared: an intermittent that says only
// FAIL captures nothing (round-1 finding 3), so `detail` is mandatory here.
const ok = (name, pass, detail) => {
  if (detail === undefined) detail = '(no detail given — fix the assertion)';
  console.log(`${pass ? 'PASS' : 'FAIL'}  ${name}   ${detail}`);
  if (!pass) failures++;
};
const dB = x => (typeof x === 'number' ? x.toFixed(1) : String(x));

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
const buses      = async () => (await rest('/api/buses')).body;
const previewBus = async () => (await buses()).find(b => b.preview);

// The meter broadcast is a consuming read, so peaks are collected over a
// window rather than sampled once.
class Meters {
  constructor(ws) {
    this.frames = [];
    ws.on('message', raw => {
      let m; try { m = JSON.parse(raw); } catch { return; }
      if (m.type === 'meters') this.frames.push(m);
    });
  }
  reset() { this.frames = []; }
  peakFor(mixerId) {
    let best = -200;
    for (const f of this.frames)
      for (const c of (f.mixer_channels || []))
        if (c.mixer_id === mixerId) best = Math.max(best, c.peak_db ?? -200);
    return best;
  }
  lanesFor(mixerId) {
    let best = null;
    for (const f of this.frames)
      for (const c of (f.mixer_channels || []))
        if (c.mixer_id === mixerId && c.lanes && c.lanes.length >= 2 &&
            c.lanes[0].peak_db > -60)
          best = [c.lanes[0].peak_db, c.lanes[1].peak_db];
    return best;
  }
  // The house is masters 0/1 ONLY.
  //
  // This used to max over every master channel, which quietly stopped meaning
  // "the house" the moment the preview bus was mapped: the reserved pair
  // carries PFL and pre-listen, so "did this change the house?" was reading
  // the headphone feed and answering yes-but-identically. Both halves of that
  // question need their own number.
  housePeak() { return this.#peakOver(i => i <= 1); }
  // The reserved pair at the top of the bus — the headphone output.
  monitorOutPeak() { return this.#peakOver(i => i >= 30); }

  #peakOver(pred) {
    let best = -200;
    for (const f of this.frames)
      for (const c of (f.master_channels || []))
        if (pred(c.index)) best = Math.max(best, c.peak_db ?? -200);
    return best;
  }
  // Which master channels carried signal. The broadcast omits silent ones, so
  // this is the set of outputs actually being driven.
  // Returns "index@dB" strings so a failure says how loud, not just where.
  activeMasters() {
    const m = new Map();
    for (const f of this.frames)
      for (const c of (f.master_channels || []))
        m.set(c.index, Math.max(m.get(c.index) ?? -200, c.peak_db ?? -200));
    return [...m].filter(([, db]) => db > -80).sort((a, b) => a[0] - b[0])
                 .map(([i, db]) => `${i}@${db.toFixed(1)}`);
  }
}
async function measure(meters, ms) { meters.reset(); await sleep(ms); }

(async () => {
  const uuid = 'item-tone-0001';

  // A document with no `buses` key at all — the migration path a pre-bus
  // project takes, and the one that has to synthesise the role holders.
  let r = await rest('/api/project/document', {
    method: 'PUT',
    body: JSON.stringify({
      name: 'pfl-e2e',
      items: [{ uuid, type: 'audio', displayName: 'Tone', mediaServerPath: WAV,
                gainDb: 0, endBehavior: 'stop' }],
    }),
  });
  ok('loaded a project with no buses key', r.status === 200, `status ${r.status}`);
  for (let i = 0; i < 80; i++) {
    const p = await rest('/api/project/progress');
    if (p.body && p.body.loading === false) break;
    await sleep(100);
  }

  // ---- 1. The preview bus's defaults -----------------------------------
  let preview = await previewBus();
  ok('a preview-role bus was synthesised', !!preview,
     preview ? `id ${preview.id}` : 'no bus with preview:true');
  ok('it has the stock id and name', preview.id === 'preview' && preview.name === 'Preview',
     `${preview.id} "${preview.name}"`);
  ok('the preview bus targets the built-in Preview Out, not another bus',
     preview.output.type === 'output' && preview.output.target === 'Preview Out',
     JSON.stringify(preview.output));
  ok('the preview bus has an engine strip', !!preview.mixerId, String(preview.mixerId));
  ok('buses report pfl', 'pfl' in preview && preview.pfl === false, `pfl ${preview.pfl}`);
  // The mixer warns off this rather than guessing from the output-name list.
  ok('an unmapped preview bus reports itself unbound', preview.bound === false,
     `bound ${preview.bound}`);
  ok('the master bus holds the house pair',
     (await buses()).some(b => b.master && JSON.stringify(b.masters) === '[0,1]'),
     JSON.stringify((await buses()).filter(b => b.master).map(b => [b.id, b.masters])));

  // ---- 2. The preview bus cannot be pointed at a bus -------------------
  // `type:"master"` is the retired spelling of "into the master bus"; the
  // API still accepts it and maps it to bus->master (D25), so on the preview
  // bus it is refused for the same reason a bus target is.
  r = await rest(`/api/buses/${preview.id}`, {
    method: 'PATCH', body: JSON.stringify({ output: { type: 'master', target: '' } }),
  });
  ok('routing the preview bus to the master bus is refused', r.status === 409,
     `status ${r.status} ${JSON.stringify(r.body)}`);
  ok('...with the preview-to-bus text',
     r.body && r.body.error === 'the Preview bus cannot be routed to another bus',
     JSON.stringify(r.body));
  preview = await previewBus();
  ok('the refused patch left the preview bus untouched',
     preview.output.type === 'output' && preview.output.target === 'Preview Out',
     JSON.stringify(preview.output));

  // ---- 3. PFL on the preview bus itself --------------------------------
  r = await rest(`/api/buses/${preview.id}/pfl`, { method: 'POST', body: JSON.stringify({ pfl: true }) });
  ok('pfl on the preview bus is refused', r.status === 404, `status ${r.status}`);

  // ---- Put the tone on a bus of its own --------------------------------
  r = await rest('/api/buses', { method: 'POST', body: JSON.stringify({ name: 'Tone', width: 2 }) });
  const busId = r.body.id;
  ok('created a test bus', !!busId, String(busId));
  await rest(`/api/project/items/${uuid}`, {
    method: 'PATCH', body: JSON.stringify({ busId }),
  });

  let bs = await buses();
  const bus = bs.find(b => b.id === busId);
  preview   = bs.find(b => b.preview);
  ok('the test bus has an engine strip', !!bus && !!bus.mixerId, String(bus && bus.mixerId));

  const ws = new WebSocket(`ws://127.0.0.1:${PORT}/ws`);
  await new Promise((res, rej) => { ws.on('open', res); ws.on('error', rej); });
  const meters = new Meters(ws);

  // ---- Establish a clean starting state ourselves ----------------------
  // A second run against the same server used to fail "preview is silent
  // before PFL": the strip and the reserved pair keep the previous run's
  // tail for tens of seconds (~4 dB/s fall-back). Asserting state instead of
  // establishing it was the bug — so clear PFL, stop any audition, and wait
  // for the preview strip to actually read silent before taking a baseline.
  await rest('/api/buses/pfl/clear', { method: 'POST', body: '{}' });
  await rest('/api/preview', { method: 'DELETE' });
  let settled = -200;
  for (let i = 0; i < 40; i++) {              // up to 40 x 500 ms = 20 s
    await measure(meters, 500);
    settled = meters.peakFor(preview.mixerId);
    if (settled < -60) break;
  }
  ok('the preview strip reads silent before the test starts', settled < -60,
     `${dB(settled)} dBFS`);

  await rest(`/api/project/items/${uuid}/play`, { method: 'POST', body: '{}' });
  await sleep(500);

  // ---- 4. Baseline -----------------------------------------------------
  await measure(meters, 800);
  const busIdle    = meters.peakFor(bus.mixerId);
  const monIdle    = meters.peakFor(preview.mixerId);
  const houseIdle  = meters.housePeak();
  ok('the bus is passing signal', busIdle > -30, `${dB(busIdle)} dBFS`);
  ok('the preview strip is silent before PFL', monIdle < -60, `${dB(monIdle)} dBFS`);
  ok('the house carries the bus', houseIdle > -30, `${dB(houseIdle)} dBFS`);

  // ---- 5. PFL up -------------------------------------------------------
  r = await rest(`/api/buses/${busId}/pfl`, { method: 'POST', body: JSON.stringify({ pfl: true }) });
  ok('pfl accepted', r.status === 200, `status ${r.status}`);
  await sleep(250);
  await measure(meters, 800);
  const monPfl   = meters.peakFor(preview.mixerId);
  const housePfl = meters.housePeak();
  ok('the preview strip hears the bus once PFL is up', monPfl > -30, `${dB(monPfl)} dBFS`);
  ok('pfl did not change the house level',
     Math.abs(housePfl - houseIdle) < 1.0,
     `${dB(houseIdle)} -> ${dB(housePfl)} dBFS`);
  // With no headphone output configured, the preview bus must be wired to
  // nothing at all. If it fell back to a device, its master pair would light
  // up here — which is the whole PFL-in-the-house failure, visible.
  //
  // Stated against the house level rather than an absolute floor: master
  // meters fall back slowly (~4 dB/s), so running this script twice in a row
  // leaves the previous run's tail on the reserved pair for tens of seconds.
  // A channel genuinely carrying the tone reads within a dB of the house; a
  // tail is tens of dB below it.
  const active = meters.activeMasters();
  const loudExtra = active
    .map(s => s.split('@'))
    .filter(([i, db]) => Number(i) > 1 && Number(db) > houseIdle - 20);
  ok('an unmapped preview bus drives no hardware output',
     active.length > 0 && loudExtra.length === 0, `masters ${active.join(' ')}`);
  bs = await buses();
  ok('pfl is reported back on the bus', bs.find(b => b.id === busId).pfl === true,
     `pfl ${bs.find(b => b.id === busId).pfl}`);

  // The peak meter has hold-and-decay ballistics, so a level that has just
  // been pulled down reads as a falling number for seconds afterwards. What
  // these assertions actually claim is a *change* of level, so they are
  // written that way rather than against an absolute floor a decaying meter
  // would not reach inside the window.
  const DROP = 12;  // dB — well beyond decay over the settle, far short of off

  // ---- 6. Pre-fader ----------------------------------------------------
  await rest(`/api/buses/${busId}`, { method: 'PATCH', body: JSON.stringify({ gainDb: -60 }) });
  await sleep(1200);
  await measure(meters, 800);
  const busFaded = meters.peakFor(bus.mixerId);
  const monFaded = meters.peakFor(preview.mixerId);
  ok('the fader takes the bus itself down',
     busFaded < busIdle - DROP, `${dB(busIdle)} -> ${dB(busFaded)} dBFS`);
  ok('PFL is PRE-FADER: the preview strip is unchanged',
     Math.abs(monFaded - monPfl) < 1.0, `${dB(monPfl)} -> ${dB(monFaded)} dBFS`);

  // ---- 7. Pre-mute -----------------------------------------------------
  await rest(`/api/buses/${busId}`, {
    method: 'PATCH', body: JSON.stringify({ gainDb: 0, mute: true }),
  });
  await sleep(1200);
  await measure(meters, 800);
  const busMuted = meters.peakFor(bus.mixerId);
  const monMuted = meters.peakFor(preview.mixerId);
  ok('mute takes the bus itself down',
     busMuted < busIdle - DROP, `${dB(busIdle)} -> ${dB(busMuted)} dBFS`);
  ok('PFL is PRE-MUTE: the preview strip is unchanged',
     Math.abs(monMuted - monPfl) < 1.0, `${dB(monPfl)} -> ${dB(monMuted)} dBFS`);

  // ---- 8. Clear --------------------------------------------------------
  await rest(`/api/buses/${busId}`, { method: 'PATCH', body: JSON.stringify({ mute: false }) });
  r = await rest('/api/buses/pfl/clear', { method: 'POST', body: '{}' });
  ok('clear reports what it cleared', r.body.cleared === 1, JSON.stringify(r.body));
  await sleep(1200);
  await measure(meters, 800);
  const monCleared = meters.peakFor(preview.mixerId);
  ok('clear takes the bus back out of the preview strip',
     monCleared < monPfl - DROP, `${dB(monPfl)} -> ${dB(monCleared)} dBFS`);
  ok('the bus itself is unaffected by clearing PFL',
     Math.abs(meters.peakFor(bus.mixerId) - busIdle) < 1.0,
     `${dB(busIdle)} -> ${dB(meters.peakFor(bus.mixerId))} dBFS`);

  // ---- 9. Mono placement ----------------------------------------------
  await rest(`/api/buses/${busId}`, { method: 'PATCH', body: JSON.stringify({ width: 1 }) });
  await rest(`/api/buses/${busId}/pfl`, { method: 'POST', body: JSON.stringify({ pfl: true }) });
  await sleep(500);
  await measure(meters, 900);
  const lanes = meters.lanesFor(preview.mixerId);
  ok('a mono bus is centred in the preview strip, not hard left',
     !!lanes && Math.abs(lanes[0] - lanes[1]) < 1.0,
     lanes ? `L ${dB(lanes[0])} / R ${dB(lanes[1])}` : 'no reading');

  // ---- 10. Mapping Preview Out wires it, on the reserved pair -----------
  // Until it is bound, the preview bus is deliberately silent. Binding it
  // must pick the bus up without a reload — the output-map edit path re-wires
  // any bus whose resolution changed, and one with no recorded resolution
  // always counts as changed. It must land on the pair the engine reserves at
  // the top of the master bus (30/31 at the default width), because that pair
  // is the headphone output: the preview bus and pre-listen are the same thing.
  const devs = (await rest('/api/devices')).body;
  const devName = Array.isArray(devs) && devs.length
    ? (devs[0].display_name || devs[0].name) : null;
  const previewMap = { version: 1, outputs: [
    { name: 'Preview Out', channels: [
      { device: devName, hwChannel: 0 }, { device: devName, hwChannel: 1 }] },
  ]};
  if (!devName) { console.log('SKIP  preview mapping (no devices enumerated)'); }
  else {
    r = await rest('/api/outputs', { method: 'PUT', body: JSON.stringify(previewMap) });
    ok('mapping Preview Out rewires exactly one bus',
       r.body && r.body.rewiredBuses === 1, JSON.stringify(r.body));
    await sleep(600);
    await measure(meters, 800);
    const nowActive = meters.activeMasters();
    ok('a mapped preview bus drives the reserved preview pair',
       nowActive.some(s => Number(s.split('@')[0]) >= 30), `masters ${nowActive.join(' ')}`);
    preview = await previewBus();
    ok('a mapped preview bus reports itself bound, on the reserved pair',
       preview.bound === true && Array.isArray(preview.masters) && preview.masters[0] >= 30,
       `bound ${preview.bound} masters ${JSON.stringify(preview.masters)}`);

    // Saving the map again with no change must not tear the headphone feed
    // down and rebuild it — that churn was audible as a gap.
    r = await rest('/api/outputs', { method: 'PUT', body: JSON.stringify(previewMap) });
    ok('re-saving an unchanged map rewires nothing',
       r.body && r.body.rewiredBuses === 0, JSON.stringify(r.body));
  }

  // ---- 11. Pre-listen shares the bus with PFL --------------------------
  // The point of the merge: auditioning a cue and PFL'ing a bus arrive in the
  // same headphones, under one fader, on one meter. Previously pre-listen had
  // a strip of its own that the mixer never showed.
  // Back to stereo first: step 9 left the bus mono, which moves its own send
  // into the master by the pan law and would show up as a house-level change
  // that has nothing to do with pre-listen.
  await rest(`/api/buses/${busId}`, { method: 'PATCH', body: JSON.stringify({ width: 2 }) });
  await rest('/api/buses/pfl/clear', { method: 'POST', body: '{}' });
  await sleep(1500);
  await measure(meters, 800);
  const monQuiet = meters.peakFor(preview.mixerId);

  r = await rest('/api/preview', { method: 'POST', body: JSON.stringify({ itemUuid: uuid }) });
  ok('pre-listen starts', r.status === 200, `status ${r.status}`);
  await sleep(900);
  await measure(meters, 900);
  const monPreview   = meters.peakFor(preview.mixerId);
  const housePreview = meters.housePeak();
  ok('pre-listen goes to the preview bus, not a strip of its own',
     monPreview > -30 && monPreview > monQuiet + 12,
     `${dB(monQuiet)} -> ${dB(monPreview)} dBFS`);
  // The house must be untouched, exactly as it is for PFL. This is the check
  // that used to read the reserved pair and answer itself.
  ok('pre-listen does not change the house level',
     Math.abs(housePreview - houseIdle) < 1.0,
     `${dB(houseIdle)} -> ${dB(housePreview)} dBFS`);
  // And it must actually leave the machine, on the reserved pair.
  ok('pre-listen reaches the headphone output',
     meters.monitorOutPeak() > -30, `${dB(meters.monitorOutPeak())} dBFS`);

  // Auditioning answers "what will this sound like when I fire it", so it has
  // to honour the item's own level. It did not: the preview loaded a fresh cue
  // and left it at unity, so trimming an item changed playback and not the
  // preview of it.
  await rest('/api/preview', { method: 'DELETE' });
  await rest(`/api/project/items/${uuid}`, {
    method: 'PATCH', body: JSON.stringify({ volume: 0.25 }),   // -12.04 dB
  });
  await rest('/api/preview', { method: 'POST', body: JSON.stringify({ itemUuid: uuid }) });
  await sleep(900);
  await measure(meters, 900);
  const monTrimmed = meters.peakFor(preview.mixerId);
  ok('pre-listen honours the item level',
     Math.abs(monTrimmed - (monPreview - 12.04)) < 1.5,
     `${dB(monPreview)} at unity -> ${dB(monTrimmed)} at -12 dB`);
  await rest(`/api/project/items/${uuid}`, {
    method: 'PATCH', body: JSON.stringify({ volume: 1 }),
  });
  await rest('/api/preview', { method: 'DELETE' });
  await rest('/api/preview', { method: 'POST', body: JSON.stringify({ itemUuid: uuid }) });
  await sleep(900);

  // ---- 12. PFL and pre-listen SUM --------------------------------------
  // The whole point of merging the two: a bus PFL'd while a cue is being
  // auditioned gives you both in one pair of headphones. Same tone at the same
  // level from both sources, so summing them is a clear step up rather than a
  // level that just stays where it was.
  await rest(`/api/buses/${busId}/pfl`, { method: 'POST', body: JSON.stringify({ pfl: true }) });
  await sleep(600);
  await measure(meters, 900);
  const monBoth = meters.peakFor(preview.mixerId);
  ok('PFL and pre-listen sum in the preview strip',
     monBoth > monPreview + 2.0,
     `preview ${dB(monPreview)} -> with PFL ${dB(monBoth)} dBFS`);
  ok('summing them still does not touch the house',
     Math.abs(meters.housePeak() - houseIdle) < 1.0,
     `${dB(houseIdle)} -> ${dB(meters.housePeak())} dBFS`);

  await rest('/api/preview', { method: 'DELETE' });
  await rest('/api/buses/pfl/clear', { method: 'POST', body: '{}' });

  // ---- 13. A device name as the target binds the preview bus -----------
  // settings.previewDevice is gone from the document (D28/D30): a project
  // that carried one has it migrated onto the preview bus as a plain device
  // name, and the strip's selector writes the same thing (D29). With NO
  // output map, that name alone has to bind the bus — and an absent name has
  // to leave it unbound and silent, never on the default device.
  if (devName) {
    await rest('/api/outputs', {
      method: 'PUT', body: JSON.stringify({ version: 1, outputs: [] }),
    });
    await sleep(700);
    ok('unmapped again, the preview bus is unbound',
       (await previewBus()).bound === false, `bound ${(await previewBus()).bound}`);
    r = await rest(`/api/buses/${preview.id}`, {
      method: 'PATCH', body: JSON.stringify({ output: { type: 'output', target: devName } }),
    });
    ok('a device name is accepted as the preview target', r.status === 200,
       `status ${r.status} ${JSON.stringify(r.body)}`);
    await sleep(700);
    const viaDevice = await previewBus();
    ok('a preview bus targeting a present device name reports itself bound',
       viaDevice.bound === true, `bound ${viaDevice.bound} target "${viaDevice.output.target}"`);
    await rest('/api/preview', { method: 'POST', body: JSON.stringify({ itemUuid: uuid }) });
    await sleep(900);
    await measure(meters, 900);
    ok('a device-named preview target drives the headphone output with no output map',
       meters.monitorOutPeak() > -30, `${dB(meters.monitorOutPeak())} dBFS`);
    ok('...and still does not touch the house',
       Math.abs(meters.housePeak() - houseIdle) < 1.0,
       `${dB(houseIdle)} -> ${dB(meters.housePeak())} dBFS`);
    await rest('/api/preview', { method: 'DELETE' });

    // A name no device on this machine answers to: unbound, and NOT the
    // default device — the identity fallback every other bus gets would put
    // every PFL'd channel in the house.
    r = await rest(`/api/buses/${preview.id}`, {
      method: 'PATCH', body: JSON.stringify({ output: { type: 'output', target: 'No Such Device (pfl-e2e)' } }),
    });
    await sleep(700);
    const ghost = await previewBus();
    ok('a preview bus targeting an absent device name reports itself unbound',
       r.status === 200 && ghost.bound === false, `status ${r.status} bound ${ghost.bound}`);
    await sleep(1500);   // let the reserved pair's meter fall away from §13's feed
    await rest('/api/preview', { method: 'POST', body: JSON.stringify({ itemUuid: uuid }) });
    await sleep(900);
    await measure(meters, 900);
    const ghostActive = meters.activeMasters()
      .map(s => s.split('@'))
      .filter(([i, db]) => Number(i) > 1 && Number(db) > houseIdle - 20);
    ok('an absent device name leaves pre-listen silent — not on the default device',
       ghostActive.length === 0, `masters ${meters.activeMasters().join(' ')}`);
    await rest('/api/preview', { method: 'DELETE' });
    await rest(`/api/buses/${preview.id}`, {
      method: 'PATCH', body: JSON.stringify({ output: { type: 'output', target: 'Preview Out' } }),
    });
  }

  // Put the output map back. It is server config that persists to disk, and
  // several assertions here depend on the preview bus starting out unmapped —
  // leaving the binding behind made the second run of this script disagree
  // with the first for reasons that had nothing to do with the code.
  await rest('/api/outputs', {
    method: 'PUT', body: JSON.stringify({ version: 1, outputs: [] }),
  });

  await rest(`/api/project/items/${uuid}/stop`, { method: 'POST', body: '{}' });
  ws.close();
  console.log(`\n${failures === 0 ? 'ALL PASS' : 'FAILURES'} (${failures})`);
  process.exit(failures === 0 ? 0 : 1);
})().catch(e => { console.error('harness error:', e); process.exit(2); });

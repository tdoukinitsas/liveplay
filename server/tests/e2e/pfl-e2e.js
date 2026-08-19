// End-to-end check of Stage 3 against a running liveplay-server.
//
// The claims are behavioural, so they are checked against real audio through
// the real render loop rather than by inspecting state:
//   1. Monitor is synthesised onto the "Monitor" logical output, not the master.
//   2. Pointing Monitor at the master is refused, and changes nothing.
//   3. PFL cannot be raised on Monitor itself.
//   4. PFL puts a bus into the monitor strip; clearing takes it out.
//   5. The tap is PRE-FADER: fader at -inf, monitor still hears it.
//   6. The tap is PRE-MUTE: bus muted, monitor still hears it.
//   7. PFL never changes what the house hears.
//   8. A mono bus arrives centred, not hard left.
//   9. With no headphone output configured, Monitor drives no hardware at all.
//  10. Configured, it drives the reserved pair at the top of the master bus.
//  11. Cue pre-listen goes to that same bus — Monitor IS the preview bus.
//  12. PFL and pre-listen sum there, which is the point of merging them.
//  13. settings.previewDevice still binds it, for projects with no output map.
//
// "The house" means masters 0/1 specifically. Maxing over every master channel
// silently stopped meaning that once Monitor was mapped, because the reserved
// pair is a master channel too — see Meters.housePeak.
//
// usage: node pfl-e2e.js <port> <wavPath>
// Resolved against the repo, not this scratch directory.
const WebSocket = require(require.resolve('ws', { paths: [process.cwd()] }));

const PORT = process.argv[2] || '4495';
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
  // "the house" the moment Monitor was mapped: the reserved pair carries
  // PFL and pre-listen, so "did this change the house?" was reading the
  // headphone feed and answering yes-but-identically. Both halves of that
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
  // project takes, and the one that has to synthesise Monitor correctly.
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

  // ---- 1. Monitor's default output -------------------------------------
  let { body: buses } = await rest('/api/buses');
  let monitor = buses.find(b => b.id === 'monitor');
  ok('monitor bus was synthesised', !!monitor);
  ok('monitor targets a logical output, not the master',
     monitor.output.type === 'output' && monitor.output.target === 'Monitor',
     JSON.stringify(monitor.output));
  ok('monitor has an engine strip', !!monitor.mixerId, monitor.mixerId);
  ok('buses report pfl', 'pfl' in monitor && monitor.pfl === false);
  // The mixer warns off this rather than guessing from the output-name list,
  // which cannot see a binding that came from settings.previewDevice.
  ok('an unconfigured monitor reports itself unbound', monitor.bound === false);

  // ---- 2. Monitor cannot be pointed at the master ----------------------
  r = await rest('/api/buses/monitor', {
    method: 'PATCH', body: JSON.stringify({ output: { type: 'master', target: '' } }),
  });
  ok('routing monitor to the master is refused', r.status === 409, `status ${r.status}`);
  ({ body: buses } = await rest('/api/buses'));
  monitor = buses.find(b => b.id === 'monitor');
  ok('the refused patch left monitor untouched',
     monitor.output.type === 'output' && monitor.output.target === 'Monitor');

  // ---- 3. PFL on monitor itself ----------------------------------------
  r = await rest('/api/buses/monitor/pfl', { method: 'POST', body: JSON.stringify({ pfl: true }) });
  ok('pfl on the monitor bus is refused', r.status === 404, `status ${r.status}`);

  // ---- Put the tone on a bus of its own --------------------------------
  r = await rest('/api/buses', { method: 'POST', body: JSON.stringify({ name: 'Tone', width: 2 }) });
  const busId = r.body.id;
  ok('created a test bus', !!busId, busId);
  await rest(`/api/project/items/${uuid}`, {
    method: 'PATCH', body: JSON.stringify({ busId }),
  });

  ({ body: buses } = await rest('/api/buses'));
  const bus = buses.find(b => b.id === busId);
  monitor   = buses.find(b => b.id === 'monitor');
  ok('the test bus has an engine strip', !!bus && !!bus.mixerId, bus && bus.mixerId);

  const ws = new WebSocket(`ws://127.0.0.1:${PORT}/ws`);
  await new Promise((res, rej) => { ws.on('open', res); ws.on('error', rej); });
  const meters = new Meters(ws);

  await rest(`/api/project/items/${uuid}/play`, { method: 'POST', body: '{}' });
  await sleep(500);

  // ---- 4. Baseline -----------------------------------------------------
  await measure(meters, 800);
  const busIdle    = meters.peakFor(bus.mixerId);
  const monIdle    = meters.peakFor(monitor.mixerId);
  const houseIdle  = meters.housePeak();
  ok('the bus is passing signal', busIdle > -30, `${busIdle.toFixed(1)} dBFS`);
  ok('monitor is silent before PFL', monIdle < -60, `${monIdle.toFixed(1)} dBFS`);

  // ---- 5. PFL up -------------------------------------------------------
  r = await rest(`/api/buses/${busId}/pfl`, { method: 'POST', body: JSON.stringify({ pfl: true }) });
  ok('pfl accepted', r.status === 200, `status ${r.status}`);
  await sleep(250);
  await measure(meters, 800);
  const monPfl   = meters.peakFor(monitor.mixerId);
  const housePfl = meters.housePeak();
  ok('monitor hears the bus once PFL is up', monPfl > -30, `${monPfl.toFixed(1)} dBFS`);
  ok('pfl did not change the house level',
     Math.abs(housePfl - houseIdle) < 1.0,
     `${houseIdle.toFixed(1)} -> ${housePfl.toFixed(1)} dBFS`);
  // With no headphone output configured, Monitor must be wired to nothing at
  // all. If it fell back to a device, its master pair would light up here —
  // which is the whole PFL-in-the-house failure, visible.
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
  ok('an unconfigured monitor drives no hardware output',
     active.length > 0 && loudExtra.length === 0, `masters ${active.join(' ')}`);
  ({ body: buses } = await rest('/api/buses'));
  ok('pfl is reported back on the bus', buses.find(b => b.id === busId).pfl === true);

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
  const monFaded = meters.peakFor(monitor.mixerId);
  ok('the fader takes the bus itself down',
     busFaded < busIdle - DROP, `${busIdle.toFixed(1)} -> ${busFaded.toFixed(1)} dBFS`);
  ok('PFL is PRE-FADER: monitor is unchanged',
     Math.abs(monFaded - monPfl) < 1.0, `${monPfl.toFixed(1)} -> ${monFaded.toFixed(1)} dBFS`);

  // ---- 7. Pre-mute -----------------------------------------------------
  await rest(`/api/buses/${busId}`, {
    method: 'PATCH', body: JSON.stringify({ gainDb: 0, mute: true }),
  });
  await sleep(1200);
  await measure(meters, 800);
  const busMuted = meters.peakFor(bus.mixerId);
  const monMuted = meters.peakFor(monitor.mixerId);
  ok('mute takes the bus itself down',
     busMuted < busIdle - DROP, `${busIdle.toFixed(1)} -> ${busMuted.toFixed(1)} dBFS`);
  ok('PFL is PRE-MUTE: monitor is unchanged',
     Math.abs(monMuted - monPfl) < 1.0, `${monPfl.toFixed(1)} -> ${monMuted.toFixed(1)} dBFS`);

  // ---- 8. Clear --------------------------------------------------------
  await rest(`/api/buses/${busId}`, { method: 'PATCH', body: JSON.stringify({ mute: false }) });
  r = await rest('/api/buses/pfl/clear', { method: 'POST', body: '{}' });
  ok('clear reports what it cleared', r.body.cleared === 1, JSON.stringify(r.body));
  await sleep(1200);
  await measure(meters, 800);
  const monCleared = meters.peakFor(monitor.mixerId);
  ok('clear takes the bus back out of the monitor',
     monCleared < monPfl - DROP, `${monPfl.toFixed(1)} -> ${monCleared.toFixed(1)} dBFS`);
  ok('the bus itself is unaffected by clearing PFL',
     Math.abs(meters.peakFor(bus.mixerId) - busIdle) < 1.0,
     `${busIdle.toFixed(1)} -> ${meters.peakFor(bus.mixerId).toFixed(1)} dBFS`);

  // ---- 9. Mono placement ----------------------------------------------
  await rest(`/api/buses/${busId}`, { method: 'PATCH', body: JSON.stringify({ width: 1 }) });
  await rest(`/api/buses/${busId}/pfl`, { method: 'POST', body: JSON.stringify({ pfl: true }) });
  await sleep(500);
  await measure(meters, 900);
  const lanes = meters.lanesFor(monitor.mixerId);
  ok('a mono bus is centred in the monitor, not hard left',
     !!lanes && Math.abs(lanes[0] - lanes[1]) < 1.0,
     lanes ? `L ${lanes[0].toFixed(1)} / R ${lanes[1].toFixed(1)}` : 'no reading');

  // ---- 10. Mapping Monitor wires it, on the reserved pair ---------------
  // Until it is bound, Monitor is deliberately silent. Binding it must pick
  // the bus up without a reload — the output-map edit path re-wires any bus
  // whose resolution changed, and one with no recorded resolution always
  // counts as changed. It must land on the pair the engine reserves at the top
  // of the master bus (30/31 at the default width), because that pair is the
  // headphone output: the preview bus and Monitor are the same thing now.
  const devs = (await rest('/api/devices')).body;
  const devName = Array.isArray(devs) && devs.length
    ? (devs[0].display_name || devs[0].name) : null;
  if (!devName) { console.log('SKIP  monitor mapping (no devices enumerated)'); }
  else {
    r = await rest('/api/outputs', {
      method: 'PUT',
      body: JSON.stringify({ version: 1, outputs: [
        { name: 'Monitor', channels: [
          { device: devName, hwChannel: 0 }, { device: devName, hwChannel: 1 }] },
      ]}),
    });
    ok('mapping Monitor rewires exactly one bus',
       r.body && r.body.rewiredBuses === 1, JSON.stringify(r.body));
    await sleep(600);
    await measure(meters, 800);
    const nowActive = meters.activeMasters();
    ok('a mapped monitor drives the reserved preview pair',
       nowActive.some(s => Number(s.split('@')[0]) >= 30), `masters ${nowActive.join(' ')}`);

    // Saving the map again with no change must not tear the headphone feed
    // down and rebuild it — that churn was audible as a gap.
    r = await rest('/api/outputs', {
      method: 'PUT',
      body: JSON.stringify({ version: 1, outputs: [
        { name: 'Monitor', channels: [
          { device: devName, hwChannel: 0 }, { device: devName, hwChannel: 1 }] },
      ]}),
    });
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
  const monQuiet = meters.peakFor(monitor.mixerId);

  r = await rest('/api/preview', { method: 'POST', body: JSON.stringify({ itemUuid: uuid }) });
  ok('pre-listen starts', r.status === 200, `status ${r.status}`);
  await sleep(900);
  await measure(meters, 900);
  const monPreview   = meters.peakFor(monitor.mixerId);
  const housePreview = meters.housePeak();
  ok('pre-listen goes to the Monitor bus, not a strip of its own',
     monPreview > -30 && monPreview > monQuiet + 12,
     `${monQuiet.toFixed(1)} -> ${monPreview.toFixed(1)} dBFS`);
  // The house must be untouched, exactly as it is for PFL. This is the check
  // that used to read the reserved pair and answer itself.
  ok('pre-listen does not change the house level',
     Math.abs(housePreview - houseIdle) < 1.0,
     `${houseIdle.toFixed(1)} -> ${housePreview.toFixed(1)} dBFS`);
  // And it must actually leave the machine, on the reserved pair.
  ok('pre-listen reaches the headphone output',
     meters.monitorOutPeak() > -30, `${meters.monitorOutPeak().toFixed(1)} dBFS`);

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
  const monTrimmed = meters.peakFor(monitor.mixerId);
  ok('pre-listen honours the item level',
     Math.abs(monTrimmed - (monPreview - 12.04)) < 1.5,
     `${monPreview.toFixed(1)} at unity -> ${monTrimmed.toFixed(1)} at -12 dB`);
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
  const monBoth = meters.peakFor(monitor.mixerId);
  ok('PFL and pre-listen sum in the monitor',
     monBoth > monPreview + 2.0,
     `preview ${monPreview.toFixed(1)} -> with PFL ${monBoth.toFixed(1)} dBFS`);
  ok('summing them still does not touch the house',
     Math.abs(meters.housePeak() - houseIdle) < 1.0,
     `${houseIdle.toFixed(1)} -> ${meters.housePeak().toFixed(1)} dBFS`);

  await rest('/api/preview', { method: 'DELETE' });
  await rest('/api/buses/pfl/clear', { method: 'POST', body: '{}' });

  // ---- 13. settings.previewDevice still binds Monitor -------------------
  // Every existing project carries this field and no output map at all. If it
  // stopped working, pre-listen would silently disappear for all of them.
  if (devName) {
    await rest('/api/outputs', {
      method: 'PUT', body: JSON.stringify({ version: 1, outputs: [] }),
    });
    await rest('/api/project/settings', {
      method: 'PATCH', body: JSON.stringify({ previewDevice: devName }),
    });
    await sleep(700);
    const viaPreviewDevice = (await rest('/api/buses')).body.find(b => b.id === 'monitor');
    ok('a monitor bound via previewDevice reports itself bound',
       viaPreviewDevice.bound === true);
    await rest('/api/preview', { method: 'POST', body: JSON.stringify({ itemUuid: uuid }) });
    await sleep(900);
    await measure(meters, 900);
    ok('settings.previewDevice binds Monitor when no output map does',
       meters.monitorOutPeak() > -30, `${meters.monitorOutPeak().toFixed(1)} dBFS`);
    await rest('/api/preview', { method: 'DELETE' });
    await rest('/api/project/settings', {
      method: 'PATCH', body: JSON.stringify({ previewDevice: '' }),
    });
  }

  // Put the output map back. It is server config that persists to disk, and
  // several assertions here depend on Monitor starting out unmapped — leaving
  // the binding behind made the second run of this script disagree with the
  // first for reasons that had nothing to do with the code.
  await rest('/api/outputs', {
    method: 'PUT', body: JSON.stringify({ version: 1, outputs: [] }),
  });

  await rest(`/api/project/items/${uuid}/stop`, { method: 'POST', body: '{}' });
  ws.close();
  console.log(`\n${failures === 0 ? 'ALL PASS' : 'FAILURES'} (${failures})`);
  process.exit(failures === 0 ? 0 : 1);
})().catch(e => { console.error('harness error:', e); process.exit(2); });

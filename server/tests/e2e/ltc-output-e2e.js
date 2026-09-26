// Timecode goes to a LOGICAL OUTPUT, and an output this machine cannot provide
// is SILENT (D38).
//
// settings.ltcDevice was the last device name a portable document carried — the
// one deliberate exception D21 left open. It was handed straight to
// ensure_device_routing(), which calls open_device_by_name(), which falls back
// to the DEFAULT device when a name matches nothing. So a show configured for
// "MOTU 8A" and opened at a venue without one did not simply lose timecode: it
// put an LTC squeal into the default device, which in a venue is the house. It
// is the §0.8 accident again, reached through the one field §0.8 did not cover.
//
// So the claims here are:
//   1. A pre-2.5 ltcDevice migrates to ltcOutput, is erased, and is reported.
//   2. An ltcOutput this machine cannot resolve reaches NO hardware at all —
//      measured on the meters, because "is it bound" is a claim about a flag
//      and the claim that matters is about where the audio went.
//   3. A mapped output DOES carry timecode, so the silence above means
//      something rather than the harness being dead.
//   4. Clearing the output takes the feed down again.
//   5. A saved project names no sound card.
//
// The cue under test sits on a bus that is itself unbound, so programme audio
// reaches nothing and anything the meters see is the LTC channel and only the
// LTC channel. That separation is the whole reason this suite can measure LTC
// at all.
//
// usage: node ltc-output-e2e.js <port> <wavPath>
const WebSocket = require(require.resolve('ws', { paths: [process.cwd()] }));
const fs   = require('fs');
const os   = require('os');
const path = require('path');

const PORT = process.argv[2] || '4490';
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

async function rest(path_, opts = {}) {
  const r = await fetch(BASE + path_, { headers: { 'content-type': 'application/json' }, ...opts });
  const text = await r.text();
  let body; try { body = JSON.parse(text); } catch { body = text; }
  return { status: r.status, body };
}
const doc      = async () => (await rest('/api/project')).body;
const settings = async () => (await doc())?.settings ?? {};
const outputs  = async () => (await rest('/api/outputs')).body;
const patchSettings = body =>
  rest('/api/project/settings', { method: 'PATCH', body: JSON.stringify(body) });
const play = uuid => rest(`/api/project/items/${uuid}/play`, { method: 'POST', body: '{}' });
const stop = uuid => rest(`/api/project/items/${uuid}/stop`, { method: 'POST', body: '{}' });

// A name no sound card has. Used both as an absent LTC output and as the
// absent target that keeps the cue's own bus silent.
const GHOST   = 'No Such Interface 9f3c2a';
const LTC_OUT = 'Timecode';
const UUID    = 'ltc-item';

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
  // Same geography as absent-device-e2e: the house is 0/1, the preview reserve
  // is 30/31 at the default width, and everything between is the pool the LTC
  // feed draws its pair from.
  house()     { return this.masterPeak(i => i <= 1); }
  reserved()  { return this.masterPeak(i => i >= 30); }
  pool()      { return this.masterPeak(i => i > 1 && i < 30); }
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
async function waitSilent(m, floor = -60, maxMs = 20000) {
  let v = -200;
  for (let t = 0; t < maxMs; t += 500) {
    await measure(m, 500);
    v = m.anyMaster();
    if (v < floor) break;
  }
  return v;
}

// The document under test. `ltcDevice` is what a pre-2.5 file carries; the
// migration is claim 1, so it goes in under the OLD key on purpose.
const projectDoc = (legacyDevice) => ({
  name: 'ltc-output-e2e',
  version: '2.0.0',
  busSchema: 2,
  settings: { ltcDevice: legacyDevice },
  items: [
    { uuid: UUID, type: 'audio', displayName: 'Timecoded', mediaServerPath: WAV,
      volume: 1, endBehavior: 'loop', busId: 'quiet',
      ltcEnabled: true, ltcStartTimecode: '01:00:00:00', ltcFrameRate: 4 },
  ],
  // The cue's own bus targets absent hardware, so programme audio reaches
  // nothing and the meters below see LTC alone. Master and preview are
  // declared so the role migration does not promote 'quiet' into the master
  // and hand it the house pair (D35), which would put programme on 0/1.
  buses: [
    { id: 'master',  name: 'Master',  width: 2, gainDb: 0, master: true,
      output: { type: 'output', target: 'Main Out' } },
    { id: 'preview', name: 'Preview', width: 2, gainDb: 0, preview: true,
      output: { type: 'output', target: 'Preview Out' } },
    { id: 'quiet',   name: 'Quiet',   width: 2, gainDb: 0,
      output: { type: 'output', target: GHOST } },
  ],
});

(async () => {
  const ws = new WebSocket(`ws://127.0.0.1:${PORT}/ws`);
  await new Promise((res, rej) => { ws.on('open', res); ws.on('error', rej); });
  const m = new Meters(ws);

  // This suite writes to outputs.json, which is machine state that outlives the
  // run. Captured now, restored in the finally.
  const originalMap = await outputs();
  const restoreMap = () => rest('/api/outputs', {
    method: 'PUT',
    body: JSON.stringify({ version: originalMap.version ?? 1,
                           outputs: originalMap.outputs ?? [] }),
  });

  try {
    const devices = (await rest('/api/devices')).body;
    if (!Array.isArray(devices) || devices.length === 0) {
      console.log('SKIP  no audio devices on this host — nothing to route timecode to');
      ws.close();
      process.exit(0);
    }
    const realDevice = devices[0].display_name || devices[0].name;

    // Start from an empty map so an inherited row for LTC_OUT cannot make the
    // absent-output case pass for the wrong reason.
    await rest('/api/outputs', { method: 'PUT', body: JSON.stringify({ version: 1, outputs: [] }) });

    // ---- 1. The migration ------------------------------------------------
    let r = await rest('/api/project/document', {
      method: 'PUT', body: JSON.stringify(projectDoc(GHOST)),
    });
    ok('a document carrying settings.ltcDevice loads', r.status === 200, `status ${r.status}`);
    ok('and the migration is reported, not silent',
       r.body?.migration?.ltcDeviceMigrated === 1,
       `migration = ${JSON.stringify(r.body?.migration ?? null)}`);
    await waitLoaded();
    await rest('/api/buses/pfl/clear', { method: 'POST', body: '{}' });
    await rest('/api/preview', { method: 'DELETE' });

    let s = await settings();
    ok('the device name became the logical output', s.ltcOutput === GHOST,
       `ltcOutput = ${JSON.stringify(s.ltcOutput)}`);
    ok('and the old key is gone, so nothing can read it again',
       s.ltcDevice === undefined,
       s.ltcDevice === undefined ? '(absent)' : `ltcDevice = ${s.ltcDevice}`);

    // ---- 2. An output this machine cannot provide is SILENT --------------
    const base = await waitSilent(m);
    ok('baseline: every master is quiet before the test signal',
       base < -60, `${dB(base)} dBFS`);

    await play(UUID);
    await measure(m, 2500);
    const ghostHouse = m.house();
    const ghostPool  = m.pool();
    const ghostRes   = m.reserved();
    // THE load-bearing assertion. Before D38 the LTC channel was handed to
    // ensure_device_routing(GHOST) → open_device_by_name(GHOST) → the DEFAULT
    // device, so timecode appeared on a pool pair physically wired to the
    // house's sound card. Against a bypassed build this reads well above -60.
    ok('timecode for an unavailable output reaches NO hardware at all',
       ghostPool < -60 && ghostRes < -60,
       `pool ${dB(ghostPool)} reserved ${dB(ghostRes)}`);
    ok('and the house pair is untouched',
       ghostHouse < -60, `house ${dB(ghostHouse)} dBFS`);
    await stop(UUID);

    // ---- 3. The contrast: a mapped output DOES carry timecode ------------
    // Without this, every silence above would also pass with a wav that never
    // loaded or a cue that never played.
    await waitSilent(m);
    await rest('/api/outputs', {
      method: 'PUT',
      body: JSON.stringify({
        version: 1,
        outputs: [{ name: LTC_OUT, channels: [
          { device: realDevice, hwChannel: 0 },
          { device: realDevice, hwChannel: 1 },
        ] }],
      }),
    });
    await patchSettings({ ltcOutput: LTC_OUT });
    s = await settings();
    ok('the project can name a mapped output', s.ltcOutput === LTC_OUT,
       `ltcOutput = ${JSON.stringify(s.ltcOutput)}`);

    await play(UUID);
    await measure(m, 2500);
    const mappedPool  = m.pool();
    const mappedHouse = m.house();
    ok('the same cue now DOES put timecode on hardware',
       mappedPool > -40, `pool ${dB(mappedPool)} dBFS`);
    // The cue's own bus is still pointed at GHOST, so the only thing that can
    // be on a master is LTC. If programme were leaking, the house would move.
    ok('and it is the LTC channel alone — programme is still going nowhere',
       mappedHouse < -60, `house ${dB(mappedHouse)} dBFS`);
    await stop(UUID);

    // ---- 4. Clearing the output takes the feed down ----------------------
    // A show that switches timecode off must stop holding the interface, not
    // just stop being asked for.
    await waitSilent(m);
    await patchSettings({ ltcOutput: null });
    s = await settings();
    ok('the output can be cleared', s.ltcOutput === null, String(s.ltcOutput));

    await play(UUID);
    await measure(m, 2500);
    ok('and timecode stops reaching hardware',
       m.anyMaster() < -60, `${dB(m.anyMaster())} dBFS`);
    await stop(UUID);

    // ---- 5. A legacy patch lands on the live key -------------------------
    // A pre-2.5 client, or a Companion button written against the old field.
    await patchSettings({ ltcDevice: LTC_OUT });
    s = await settings();
    ok('a legacy ltcDevice patch is applied as ltcOutput',
       s.ltcOutput === LTC_OUT, `ltcOutput = ${JSON.stringify(s.ltcOutput)}`);
    ok('and does not store a second copy beside it',
       s.ltcDevice === undefined,
       s.ltcDevice === undefined ? '(absent)' : `ltcDevice = ${s.ltcDevice}`);

    // ---- 6. The saved file names no sound card ---------------------------
    const dir  = fs.mkdtempSync(path.join(os.tmpdir(), 'liveplay-ltc-'));
    const file = path.join(dir, 'Show.liveplay');
    r = await rest('/api/project/save', {
      method: 'POST', body: JSON.stringify({ path: file.replace(/\\/g, '/') }),
    });
    ok('the project saves', r.status === 200, `status ${r.status}`);
    const saved = JSON.parse(fs.readFileSync(file, 'utf8'));
    ok('the saved settings carry ltcOutput',
       saved.settings?.ltcOutput === LTC_OUT,
       `ltcOutput = ${JSON.stringify(saved.settings?.ltcOutput)}`);
    ok('and carry no ltcDevice at all',
       !Object.prototype.hasOwnProperty.call(saved.settings ?? {}, 'ltcDevice'),
       saved.settings?.ltcDevice === undefined
         ? '(absent)' : `ltcDevice = ${saved.settings.ltcDevice}`);
    try { fs.rmSync(dir, { recursive: true, force: true }); } catch { /* best effort */ }
  } finally {
    await restoreMap();
    ws.close();
  }

  console.log(`\n${failures === 0 ? 'ALL PASS' : 'FAILURES'} (${failures})`);
  process.exit(failures === 0 ? 0 : 1);
})().catch(e => { console.error('harness error:', e); process.exit(2); });

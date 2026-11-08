// Volume automation, duck ramps and the Stop All item-fade mode (2.6:
// #78, #85 and the automation envelope), end-to-end against a running server.
//
// Every level is read off the cue's own source meter (post item gain,
// pre-bus), so bus DSP and the limiter can't make a wrong answer look right.
// The signal is a steady 440 Hz tone at -6 dBFS peak, so any change in level
// is the server's doing.
//
//   automation: a 0 dB -> -20 dB dip at 2-2.5 s is heard at the playhead
//               positions it was drawn at; bypassed it changes nothing; an
//               edit lands on a cue that is already playing; Preview from
//               cursor starts the audition where it was asked to.
//   #78:        a duck with duckFadeIn/duckFadeOut of 1 s is part-way down
//               after 0.3 s and fully down after 1.5 s; and back the same way.
//               Without the fields (pre-2.6 items) it is down at once.
//   #85:        with stopAllUsesItemFade, Stop All fades each cue over its own
//               Stop button fade (a cue with none takes the Stop All fade);
//               without it, everything takes the Stop All fade. The liveplay
//               events plugin's stop-fade-mode action flips the setting.
//
// usage: node automation-e2e.js <port>      (never 4480: that is the live app)
const fs   = require('fs');
const os   = require('os');
const path = require('path');
const WebSocket = require(require.resolve('ws', { paths: [process.cwd()] }));

const PORT = process.argv[2];
if (!PORT || PORT === '4480') { console.error('usage: node automation-e2e.js <port other than 4480>'); process.exit(2); }
const BASE = `http://127.0.0.1:${PORT}`;
let failures = 0;
const ok = (name, pass, detail) => {
  console.log(`${pass ? 'PASS' : 'FAIL'}  ${name}   ${detail}`);
  if (!pass) failures++;
};
const sleep = ms => new Promise(r => setTimeout(r, ms));
async function rest(p, opts = {}) {
  const r = await fetch(BASE + p, { headers: { 'content-type': 'application/json' }, ...opts });
  const text = await r.text();
  let body; try { body = JSON.parse(text); } catch { body = text; }
  return { status: r.status, body };
}
const post = (p, body = {}) => rest(p, { method: 'POST', body: JSON.stringify(body) });
const patch = (p, body) => rest(p, { method: 'PATCH', body: JSON.stringify(body) });

function writeTone(file, seconds) {
  const rate = 48000, ch = 2, n = rate * seconds;
  const data = Buffer.alloc(n * ch * 2);
  for (let i = 0; i < n; i++) {
    const v = Math.round(Math.sin(2 * Math.PI * 440 * i / rate) * 0.5 * 32767);
    data.writeInt16LE(v, i * 4); data.writeInt16LE(v, i * 4 + 2);
  }
  const h = Buffer.alloc(44);
  h.write('RIFF', 0); h.writeUInt32LE(36 + data.length, 4); h.write('WAVE', 8);
  h.write('fmt ', 12); h.writeUInt32LE(16, 16); h.writeUInt16LE(1, 20); h.writeUInt16LE(ch, 22);
  h.writeUInt32LE(rate, 24); h.writeUInt32LE(rate * ch * 2, 28); h.writeUInt16LE(ch * 2, 32);
  h.writeUInt16LE(16, 34); h.write('data', 36); h.writeUInt32LE(data.length, 40);
  fs.writeFileSync(file, Buffer.concat([h, data]));
}

// Per-cue meter frames, keyed by cue id: { t, peak, playhead }.
const frames = new Map();
function onMeters(raw) {
  let m; try { m = JSON.parse(raw); } catch { return; }
  if (m.type !== 'meters') return;
  for (const it of (m.items || m.item_meters || m.cues || [])) {
    if (!it.sources) continue;
    const id = it.cue_id || it.id || it.cueId;
    const peak = Math.max(...it.sources.map(s => s.peak_max_db ?? s.peak_db ?? -200));
    if (!frames.has(id)) frames.set(id, []);
    frames.get(id).push({ t: Date.now(), peak, playhead: it.playhead_seconds });
  }
}
const framesOf = id => frames.get(id) || [];
const clearFrames = () => frames.clear();
// Median peak of the frames whose playhead lies in [a, b].
function levelAtPlayhead(id, a, b) {
  const v = framesOf(id).filter(f => f.playhead >= a && f.playhead <= b).map(f => f.peak).sort((x, y) => x - y);
  return v.length ? v[v.length >> 1] : NaN;
}
// Median peak of the frames received in the wall-clock window [t0, t1].
function levelBetween(id, t0, t1) {
  const v = framesOf(id).filter(f => f.t >= t0 && f.t <= t1).map(f => f.peak).sort((x, y) => x - y);
  return v.length ? v[v.length >> 1] : NaN;
}
const fmt = db => (Number.isFinite(db) ? `${db.toFixed(1)} dB` : String(db));

(async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'lp-auto-'));
  const tone = path.join(dir, 'tone.wav'); writeTone(tone, 8);
  const base = { type: 'audio', mediaServerPath: tone, endBehavior: { action: 'nothing' },
                 duckingBehavior: { mode: 'no-ducking' }, playFade: 0, stopFade: 0,
                 manualStopFade: 0, fadeOutDuration: 0 };
  const dip = { enabled: true, points: [{ t: 0, db: 0 }, { t: 2, db: 0 }, { t: 2.5, db: -20 }, { t: 8, db: -20 }] };
  let r = await rest('/api/project/document', { method: 'PUT', body: JSON.stringify({
    name: 'automation-e2e',
    settings: { stopAllFadeMs: 200 },
    items: [
      { ...base, uuid: 'au-dip',    displayName: 'Dip',      volumeAutomation: dip },
      { ...base, uuid: 'au-bypass', displayName: 'Bypassed', volumeAutomation: { ...dip, enabled: false } },
      { ...base, uuid: 'au-live',   displayName: 'Live edit' },
      { ...base, uuid: 'au-bed',    displayName: 'Bed' },
      { ...base, uuid: 'au-duck',   displayName: 'Ducker (1 s ramps)',
        duckingBehavior: { mode: 'duck-others', duckLevel: 0.1, duckFadeIn: 1.0, duckFadeOut: 1.0 } },
      { ...base, uuid: 'au-snap',   displayName: 'Ducker (legacy)',
        duckingBehavior: { mode: 'duck-others', duckLevel: 0.1 } },
      { ...base, uuid: 'au-long',   displayName: 'Own fade 2 s', manualStopFade: 2.0 },
      { ...base, uuid: 'au-none',   displayName: 'No own fade',  manualStopFade: 0 },
    ],
  }) });
  ok('loaded the test project', r.status === 200, `status ${r.status}`);
  for (let i = 0; i < 100; i++) {
    if ((await rest('/api/project/progress')).body.loading === false) break;
    await sleep(100);
  }
  const items = (await rest('/api/project/items?offset=0&limit=50')).body.items;
  const cueOf = Object.fromEntries(items.map(i => [i.uuid, i.cueId]));
  ok('every test cue loaded', Object.keys(cueOf).length >= 8 && Object.values(cueOf).every(Boolean),
     JSON.stringify(cueOf));

  const ws = new WebSocket(`ws://127.0.0.1:${PORT}/ws`);
  await new Promise((res, rej) => { ws.once('open', res); ws.once('error', rej); });
  ws.on('message', onMeters);
  const stopAllNow = () => post('/api/transport/stop_all', { fade_ms: 0 });

  // ---- Automation -----------------------------------------------------------
  await post('/api/project/items/au-dip/play');
  await sleep(3800);
  await stopAllNow();
  const before = levelAtPlayhead(cueOf['au-dip'], 0.3, 1.8);
  const after  = levelAtPlayhead(cueOf['au-dip'], 2.7, 3.6);
  ok('automation: before the dip the tone is at its own level (~-6 dB)', Math.abs(before + 6) < 1.5, fmt(before));
  ok('automation: after the dip it is 20 dB down (~-26 dB)', Math.abs(after + 26) < 2, fmt(after));
  const mid = levelAtPlayhead(cueOf['au-dip'], 2.2, 2.3);
  ok('automation: part-way through the ramp it is in between', mid < before - 3 && mid > after + 3, fmt(mid));

  await sleep(300); clearFrames();
  await post('/api/project/items/au-bypass/play');
  await sleep(3500);
  await stopAllNow();
  const byp = levelAtPlayhead(cueOf['au-bypass'], 2.7, 3.4);
  ok('bypassed automation changes nothing (still ~-6 dB after 2.5 s)', Math.abs(byp + 6) < 1.5, fmt(byp));

  await sleep(300); clearFrames();
  await post('/api/project/items/au-live/play');
  await sleep(1000);
  const t0 = Date.now();
  await patch('/api/project/items/au-live', { volumeAutomation: { enabled: true, points: [{ t: 0, db: -12 }] } });
  await sleep(1200);
  await stopAllNow();
  const liveBefore = levelBetween(cueOf['au-live'], t0 - 700, t0 - 100);
  const liveAfter  = levelBetween(cueOf['au-live'], t0 + 400, t0 + 1100);
  ok('an automation edit reaches a cue that is already playing (-12 dB)',
     Math.abs((liveAfter - liveBefore) + 12) < 2, `${fmt(liveBefore)} -> ${fmt(liveAfter)}`);

  // ---- Preview from cursor --------------------------------------------------
  await sleep(300); clearFrames();
  r = await post('/api/preview', { itemUuid: 'au-dip', startSeconds: 5 });
  if (r.status === 200 && r.body.cueId) {
    await sleep(900);
    const pf = framesOf(r.body.cueId);
    const firstPh = pf.length ? Math.min(...pf.map(f => f.playhead)) : NaN;
    ok('preview from cursor starts at the cursor, not the in-point', firstPh >= 4.9, `first playhead ${firstPh}`);
    const pv = pf.length ? pf[pf.length - 1].peak : NaN;
    ok('the preview hears the automation too (~-26 dB at 5 s)', Math.abs(pv + 26) < 2.5, fmt(pv));
    await rest('/api/preview', { method: 'DELETE' });
  } else {
    ok('preview from cursor started', false, `status ${r.status} ${JSON.stringify(r.body)}`);
  }

  // ---- #78 duck attack / release -------------------------------------------
  async function duckRun(ducker) {
    await sleep(300); clearFrames();
    await post('/api/project/items/au-bed/play');
    await sleep(800);
    const start = Date.now();
    await post(`/api/project/items/${ducker}/play`);
    await sleep(1800);
    const stop = Date.now();
    await post(`/api/project/items/${ducker}/stop`, { fade_ms: 0 });
    await sleep(1800);
    await stopAllNow();
    const bed = cueOf['au-bed'];
    return {
      pre:      levelBetween(bed, start - 600, start - 50),
      attack03: levelBetween(bed, start + 250, start + 400),
      down:     levelBetween(bed, start + 1300, start + 1750),
      rel03:    levelBetween(bed, stop + 250, stop + 400),
      up:       levelBetween(bed, stop + 1300, stop + 1750),
    };
  }
  const d = await duckRun('au-duck');
  ok('#78: before the duck the bed is at its own level', Math.abs(d.pre + 6) < 1.5, fmt(d.pre));
  ok('#78: 0.3 s into a 1 s duck the bed is only part-way down',
     d.attack03 < d.pre - 2 && d.attack03 > -24, fmt(d.attack03));
  ok('#78: after 1.5 s it is fully ducked (-20 dB)', Math.abs(d.down - (d.pre - 20)) < 2.5, fmt(d.down));
  ok('#78: 0.3 s into a 1 s release it is only part-way back',
     d.rel03 > d.down + 2 && d.rel03 < d.pre - 2, fmt(d.rel03));
  ok('#78: after 1.5 s it is back', Math.abs(d.up - d.pre) < 1.5, fmt(d.up));

  const s = await duckRun('au-snap');
  ok('#78 control: without duck times (pre-2.6 item) it is down within 0.3 s',
     Math.abs(s.attack03 - (s.pre - 20)) < 2.5, fmt(s.attack03));

  // ---- #85 Stop All fade mode -----------------------------------------------
  async function stopAllRun() {
    await sleep(400); clearFrames();
    await post('/api/project/items/au-long/play');
    await post('/api/project/items/au-none/play');
    await sleep(800);
    const t = Date.now();
    await post('/api/transport/stop_all');   // no fade_ms: the project's rules
    await sleep(1200);
    // Wait out the double-tap window before the next Stop All, or it's a panic.
    await sleep(2500);
    return { long: levelBetween(cueOf['au-long'], t + 500, t + 800),
             none: levelBetween(cueOf['au-none'], t + 500, t + 800) };
  }
  let sa = await stopAllRun();
  ok('#85 control: by default Stop All fades everything over 200 ms',
     !(sa.long > -40), `own-fade cue at +0.5 s: ${fmt(sa.long)}`);

  await patch('/api/project/settings', { stopAllUsesItemFade: true });
  sa = await stopAllRun();
  ok('#85: with the setting on, a cue with a 2 s Stop fade is still fading at +0.5 s',
     sa.long > -20, fmt(sa.long));
  ok('#85: and a cue with no Stop fade of its own takes the 200 ms Stop All fade',
     !(sa.none > -40), fmt(sa.none));

  // The events action, which flips the same setting.
  const fire = async (mode) => (await post('/api/events/test',
    { event: { plugin: 'liveplay', action: 'stop-fade-mode', options: { mode } } })).body;
  const setting = async () => (await rest('/api/project')).body?.settings?.stopAllUsesItemFade;
  await fire('toggle');
  ok('events: stop-fade-mode toggle turns it off', (await setting()) === false, String(await setting()));
  await fire('item');
  ok('events: stop-fade-mode item turns it on', (await setting()) === true, String(await setting()));
  await fire('global');
  ok('events: stop-fade-mode global turns it off', (await setting()) === false, String(await setting()));

  ws.close();
  await post('/api/project/close');
  fs.rmSync(dir, { recursive: true, force: true });
  console.log(failures ? `\nFAILURES: ${failures}` : '\nALL PASS');
  process.exit(failures ? 1 : 0);
})().catch(e => { console.error(e); process.exit(2); });

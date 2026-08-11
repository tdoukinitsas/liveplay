// Does editing an item's properties disturb playing audio?
//
// The report: any UI action that changes any track's properties causes a pop —
// colour, trim, end behaviour — and it does not have to be the track that is
// playing. Colour is the interesting one, because nothing about a colour
// reaches the audio engine, so if it pops then the cause is the machinery
// around the edit rather than the edit itself.
//
// An audible pop from a control action is one of two things, and they need
// different fixes:
//
//   UNDERRUN — the render thread was blocked long enough for the device to run
//     dry. Shows up in `underruns`. Cause is contention: the render thread
//     takes the engine mutex twice per block, so anything holding it for long
//     enough stalls audio.
//
//   DISCONTINUITY — the audio kept flowing but a value jumped. No underrun,
//     but a step in the waveform. Shows up as a peak well above the steady
//     level of a signal we know to be constant.
//
// So this measures both, against a baseline of the same window with no edits.
//
// The item count matters more than the edit rate. Every save re-mirrors EVERY
// item to the engine, so the cost of an edit scales with the size of the show
// file rather than with what was edited — which is why a two-item probe found
// nothing and a real project misbehaves.
//
// usage: node ui-churn-probe.js <port> <wavPath> [editsPerSec] [seconds] [items]
const WebSocket = require(require.resolve('ws', { paths: [process.cwd()] }));
const PORT = process.argv[2], WAV = process.argv[3];
const RATE = Number(process.argv[4] || 10);
const SECS = Number(process.argv[5] || 6);
const ITEMS = Number(process.argv[6] || 2);
const BASE = `http://127.0.0.1:${PORT}`;

const rest = async (p, o = {}) => {
  const r = await fetch(BASE + p, { headers: { 'content-type': 'application/json' }, ...o });
  const t = await r.text();
  try { return { status: r.status, body: JSON.parse(t) }; } catch { return { status: r.status, body: t }; }
};
const sleep = ms => new Promise(r => setTimeout(r, ms));

const PLAYING = 'churn-playing';
const IDLE    = 'churn-idle';

(async () => {
  await rest('/api/project/document', {
    method: 'PUT',
    body: JSON.stringify({ name: 'churn', items: [
      { uuid: PLAYING, type: 'audio', displayName: 'Playing', mediaServerPath: WAV,
        volume: 1, endBehavior: 'loop' },
      { uuid: IDLE, type: 'audio', displayName: 'Idle', mediaServerPath: WAV,
        volume: 1, endBehavior: 'stop' },
      // Filler, to make the show file a realistic size. They are never played;
      // they exist because the mirror walks all of them on every save.
      ...Array.from({ length: Math.max(0, ITEMS - 2) }, (_, i) => ({
        uuid: `churn-fill-${String(i).padStart(3, '0')}`,
        type: 'audio', displayName: `Fill ${i}`, mediaServerPath: WAV,
        volume: 1, endBehavior: 'stop',
      })),
    ] }),
  });
  for (let i = 0; i < 200; i++) {
    const p = await rest('/api/project/progress');
    if (p.body && p.body.loading === false) break;
    await sleep(100);
  }

  const ws = new WebSocket(`ws://127.0.0.1:${PORT}/ws`);
  await new Promise((res, rej) => { ws.on('open', res); ws.on('error', rej); });
  // The signal is a constant -6 dBFS sweep, so every meter frame should read
  // -6.02. Two ways that can break, and BOTH have to be watched:
  //
  //   a peak ABOVE it  — a step discontinuity, the classic click
  //   a peak BELOW it  — a gap: audio briefly stopped arriving, which is
  //                      equally a pop and is what an "is anything too loud"
  //                      check misses completely
  //
  // The first version of this probe only tracked the maximum and reported
  // everything as clean. A dropout is silence, and silence is not loud.
  let peak = -200;
  let dip  = 200;
  let frames = 0;
  ws.on('message', raw => {
    let m; try { m = JSON.parse(raw); } catch { return; }
    if (m.type !== 'meters') return;
    for (const c of (m.master_channels || [])) {
      if (c.index !== 0) continue;
      const v = c.peak_max_db ?? -200;
      peak = Math.max(peak, v);
      dip  = Math.min(dip, v);
      frames++;
    }
  });

  await rest(`/api/project/items/${PLAYING}/play`, { method: 'POST', body: '{}' });
  await sleep(2000);

  const window = async (label, edit) => {
    await rest('/api/engine/stats?reset=1');
    peak = -200; dip = 200; frames = 0;
    const t0 = Date.now();
    let edits = 0;
    while ((Date.now() - t0) / 1000 < SECS) {
      if (edit) { await edit(edits++); }
      await sleep(1000 / RATE);
    }
    const s = (await rest('/api/engine/stats')).body;
    const bad = dip < -7.5 || peak > -5.5;
    console.log(`${label.padEnd(26)} under ${String(s.underruns).padStart(3)}  ` +
                `rmax ${s.renderBlockUsMax.toFixed(0).padStart(6)}us  ` +
                `peak ${peak.toFixed(2).padStart(7)}  dip ${dip.toFixed(2).padStart(8)}  ` +
                `frames ${String(frames).padStart(4)}  ${bad ? '<<< GLITCH' : ''}`);
    return s;
  };

  console.log(`\n${SECS}s windows, ${RATE} edits/s, one file looping throughout\n`);

  await window('baseline (no edits)', null);

  // Colour on the IDLE item: nothing about this can reach the audio engine.
  const colours = ['#ff0000', '#00ff00', '#0000ff', '#ffff00'];
  await window('colour, idle item', async (i) => {
    await rest(`/api/project/items/${IDLE}`, {
      method: 'PATCH', body: JSON.stringify({ color: colours[i % colours.length] }) });
  });

  // Colour on the PLAYING item: same edit, but now on the cue making sound.
  await window('colour, playing item', async (i) => {
    await rest(`/api/project/items/${PLAYING}`, {
      method: 'PATCH', body: JSON.stringify({ color: colours[i % colours.length] }) });
  });

  // End behaviour: reaches the sequencer, but not the sample path.
  await window('endBehavior, playing item', async (i) => {
    await rest(`/api/project/items/${PLAYING}`, {
      method: 'PATCH', body: JSON.stringify({ endBehavior: i % 2 ? 'loop' : 'stop' }) });
  });

  // Trim points: these DO reach the engine's out point.
  await window('trim, playing item', async (i) => {
    await rest(`/api/project/items/${PLAYING}`, {
      method: 'PATCH', body: JSON.stringify({ outPoint: 100 + (i % 5) }) });
  });

  // And now the thing the app actually does. Autosave is on by default, so
  // every one of the edits above is followed by POST /api/project/save with
  // the WHOLE document attached — which replaces the in-memory document and
  // re-mirrors every item to the engine. The edits alone are cheap; this is
  // the part the probe was missing.
  const doc = (await rest('/api/project')).body;
  await window('save (full document)', async (i) => {
    doc.items[1].color = colours[i % colours.length];
    await rest('/api/project/save', {
      method: 'POST', body: JSON.stringify({ document: doc }) });
  });

  // The same save WITHOUT the document body, which skips replace_full_document
  // entirely. If this one is quiet and the one above is not, the document
  // round-trip is the cause rather than writing the file.
  await window('save (no document)', async () => {
    await rest('/api/project/save', { method: 'POST', body: '{}' });
  });

  await rest(`/api/project/items/${PLAYING}`, {
    method: 'PATCH', body: JSON.stringify({ endBehavior: 'loop', outPoint: 0 }) });
  await rest(`/api/project/items/${PLAYING}/stop`, { method: 'POST', body: '{}' });
  ws.close();
})().catch(e => { console.error('probe error:', e); process.exit(2); });

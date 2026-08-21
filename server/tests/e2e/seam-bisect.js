// One save, one answer.
//
// The churn probe showed discontinuities during a window of many saves, which
// establishes correlation but not much else. This does the smallest possible
// experiment: play, sit quiet and confirm zero seams, then perform exactly ONE
// full-document save and look again. If the count moves, that single save
// caused it.
//
// usage: node seam-bisect.js <port> <wavPath> [repeats]
const fs = require('fs');
const path = require('path');
const os = require('os');
const PORT = process.argv[2], WAV = process.argv[3];
const REPEATS = Number(process.argv[4] || 5);
const BASE = `http://127.0.0.1:${PORT}`;

const rest = async (p, o = {}) => {
  const r = await fetch(BASE + p, { headers: { 'content-type': 'application/json' }, ...o });
  const t = await r.text();
  try { return { status: r.status, body: JSON.parse(t) }; } catch { return { status: r.status, body: t }; }
};
const sleep = ms => new Promise(r => setTimeout(r, ms));
const seams = async () => (await rest('/api/engine/stats')).body.discontinuities;

// Both "save" trials below only mean something if the save actually reaches
// replace_full_document. With no project path ever set the server 400s every
// one of them with "no path set" and the trial measures nothing while
// reporting clean — which is what produced the misleading "16 saves, 0 seams"
// history. So a real path is established up front, and every save is checked
// and aborts the whole run loudly if it is ever rejected.
const PROJECT_DIR = fs.mkdtempSync(path.join(os.tmpdir(), 'liveplay-seam-'));
const PROJECT_PATH = path.join(PROJECT_DIR, 'seam.liveplay');
const save = async (body) => {
  const r = await rest('/api/project/save', { method: 'POST', body: JSON.stringify(body) });
  if (r.status !== 200 || !r.body || r.body.ok !== true) {
    console.error(`\nFATAL: /api/project/save failed (${r.status}) ${JSON.stringify(r.body)} — ` +
                  `the save trials would silently measure nothing.`);
    process.exit(1);
  }
  return r;
};

// The playing cue's identity and position. If a seam coincides with the cue id
// changing, the item was reloaded underneath the audio; if the position jumps
// backwards, its decoder was seeked. Either is a step, and they need different
// fixes, so the probe records both rather than guessing which.
const cueState = async () => {
  const cues = (await rest('/api/cues')).body;
  const c = Array.isArray(cues) ? cues.find(x => x.playhead_seconds !== undefined) : null;
  return c ? { id: c.id ?? c.cue_id ?? '?', pos: c.playhead_seconds ?? -1 }
           : { id: '?', pos: -1 };
};

(async () => {
  await rest('/api/project/document', {
    method: 'PUT',
    body: JSON.stringify({ name: 'seam', items: [
      { uuid: 'seam-play', type: 'audio', displayName: 'Playing',
        mediaServerPath: WAV, volume: 1, endBehavior: 'loop' },
    ] }),
  });
  for (let i = 0; i < 200; i++) {
    const p = await rest('/api/project/progress');
    if (p.body && p.body.loading === false) break;
    await sleep(100);
  }
  // Establish a real project path so the save trials below actually reach
  // replace_full_document instead of 400ing.
  await save({ path: PROJECT_PATH });

  await rest('/api/project/items/seam-play/play', { method: 'POST', body: '{}' });
  await sleep(2500);

  const doc = (await rest('/api/project')).body;

  const trial = async (label, action) => {
    await rest('/api/engine/stats?reset=1');
    await sleep(1500);
    const quiet = await seams();
    const before = await cueState();
    await action();
    await sleep(1500);
    const after = await seams();
    const now = await cueState();
    // Expected forward progress over the ~1.5 s window, so a backward jump or
    // a stall stands out from ordinary playback.
    const drift = (now.pos - before.pos).toFixed(2);
    console.log(`${label.padEnd(30)} quiet ${String(quiet).padStart(3)}  ` +
                `after ${String(after).padStart(3)}  ` +
                `cue ${now.id === before.id ? 'same' : 'CHANGED'}  ` +
                `pos +${drift}s  ` +
                `${after > quiet ? '<<< CAUSED ' + (after - quiet) : 'clean'}`);
    return after - quiet;
  };

  for (let i = 0; i < REPEATS; i++) {
    await trial('nothing at all', async () => {});
    await trial('save WITHOUT document', async () => {
      await save({});
    });
    await trial('save WITH document', async () => {
      await save({ document: doc });
    });
    await trial('PUT document (no save)', async () => {
      await rest('/api/project/document', {
        method: 'PUT', body: JSON.stringify(doc) });
    });
  }

  await rest('/api/project/items/seam-play/stop', { method: 'POST', body: '{}' });
})().catch(e => { console.error('probe error:', e); process.exit(2); });

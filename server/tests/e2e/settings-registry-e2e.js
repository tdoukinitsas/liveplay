// The project document's `settings` object is validated against a registry
// (project_state.cpp, "The settings registry"). Before it existed,
// patch_settings() looped over the incoming patch and wrote every key straight
// into the show document — so any client could persist any key of any type
// into a .liveplay, and only a handful of keys had server meaning.
//
// Two halves to pin, and the second is the one that bites:
//
//   1. Invalid input does not land — unknown keys, wrong types, bad enum
//      values, out-of-range numbers.
//   2. Invalid input does not BREAK anything. The client watches its whole
//      settings object and PATCHes all of it back on every edit, and the object
//      it holds is the one full_document() decorated with the derived
//      outputTargetLevels. So a stray key must still return 200, the good keys
//      in the same patch must still land, and outputTargetLevels must still be
//      present on every read — while never being written to disk.
//
// Needs no audio device: nothing here plays.
//
//   node server/tests/e2e/settings-registry-e2e.js 4500
//
// Mutates server state: PUTs a fresh document and saves a project into the
// OS temp dir, which sets the server's project file path. Run it before or
// after the audio suites, not in the middle of one.
const fs   = require('fs');
const os   = require('os');
const path = require('path');

const PORT = process.argv[2];
const BASE = `http://127.0.0.1:${PORT}`;
let failures = 0;
const ok = (n, p, d = '') => { console.log(`${p ? 'PASS' : 'FAIL'}  ${n}${d ? '   ' + d : ''}`); if (!p) failures++; };
const rest = async (p, o = {}) => {
  const r = await fetch(BASE + p, { headers: { 'content-type': 'application/json' }, ...o });
  const t = await r.text();
  try { return { status: r.status, body: JSON.parse(t) }; } catch { return { status: r.status, body: t }; }
};
const patch = (o) => rest('/api/project/settings', { method: 'PATCH', body: JSON.stringify(o) });
const settings = async () => (await rest('/api/project')).body.settings ?? {};

(async () => {
  const dir  = fs.mkdtempSync(path.join(os.tmpdir(), 'liveplay-settings-'));
  const file = path.join(dir, 'settings-probe.liveplay');

  // A fresh document, so nothing carries over from a previous script.
  await rest('/api/project/document', {
    method: 'PUT',
    body: JSON.stringify({ name: 'settingsProbe', busSchema: 2, items: [], settings: {} }),
  });

  // ---- 1. The happy path still works -----------------------------------
  let r = await patch({ outputTarget: 'live', uiScrollToPlaying: true, stopAllFadeMs: 2500 });
  let s = await settings();
  ok('a valid patch returns 200', r.status === 200, `status ${r.status}`);
  ok('an enum value is stored', s.outputTarget === 'live', String(s.outputTarget));
  ok('a boolean is stored', s.uiScrollToPlaying === true, String(s.uiScrollToPlaying));
  ok('a number in range is stored', s.stopAllFadeMs === 2500, String(s.stopAllFadeMs));

  // ---- 2. Unknown keys are dropped, and do not fail the patch ----------
  // The regression this guards: answering a stray key with 400 would fail
  // every settings edit in the app, because the client sends the whole object.
  r = await patch({ notASetting: 'whatever', alsoNotOne: 42, uiScrollToPlaying: false });
  s = await settings();
  ok('a patch carrying unknown keys still returns 200', r.status === 200, `status ${r.status}`);
  ok('an unknown string key is not stored', s.notASetting === undefined, JSON.stringify(s.notASetting));
  ok('an unknown number key is not stored', s.alsoNotOne === undefined, JSON.stringify(s.alsoNotOne));
  ok('the good key in the same patch still landed', s.uiScrollToPlaying === false,
     String(s.uiScrollToPlaying));

  // ---- 3. Wrong types are dropped and the previous value survives ------
  // Dropping must leave the old value, not clear it: a client sending one bad
  // field should not silently reset a setting it never meant to touch.
  await patch({ outputTarget: 'radio' });
  r = await patch({ outputTarget: 12345, uiScrollToPlaying: 'yes' });
  s = await settings();
  ok('a wrongly-typed enum is dropped', s.outputTarget === 'radio', String(s.outputTarget));
  ok('a wrongly-typed boolean is dropped', s.uiScrollToPlaying === false, String(s.uiScrollToPlaying));

  // ---- 4. A bad enum VALUE is dropped ----------------------------------
  r = await patch({ outputTarget: 'not-a-standard' });
  s = await settings();
  ok('an out-of-vocabulary enum value is dropped', s.outputTarget === 'radio', String(s.outputTarget));

  // ---- 5. Numbers clamp rather than drop -------------------------------
  // Matching merge_bus_dsp()'s convention for every other client number.
  await patch({ stopAllFadeMs: 999999 });
  s = await settings();
  ok('a number above its range clamps to the maximum', s.stopAllFadeMs === 60000,
     String(s.stopAllFadeMs));
  await patch({ stopAllFadeMs: -5 });
  s = await settings();
  ok('a number below its range clamps to the minimum', s.stopAllFadeMs === 0,
     String(s.stopAllFadeMs));
  await patch({ indexDisplayStart: 3.7 });
  s = await settings();
  ok('a fractional integer setting truncates', s.indexDisplayStart === 3,
     String(s.indexDisplayStart));

  // ---- 6. meterBallisticsCustom clamps its sub-fields ------------------
  await patch({ meterBallistics: 'custom',
                meterBallisticsCustom: { attackMs: -50, releaseMs: 99999, rmsWindowMs: 0 } });
  s = await settings();
  const c = s.meterBallisticsCustom ?? {};
  ok('custom ballistics attack clamps up to its floor', c.attackMs === 0, String(c.attackMs));
  ok('custom ballistics release clamps to its ceiling', c.releaseMs === 10000, String(c.releaseMs));
  ok('custom ballistics RMS window clamps to its floor', c.rmsWindowMs === 1, String(c.rmsWindowMs));

  // ---- 7. A dropped key must not fire its side effect -------------------
  // patch_settings() applies limiter / ceiling / ballistics changes live. If
  // validation ran after the side effect, a rejected value would still be
  // HEARD while never being stored — the engine and the document would
  // disagree, which is exactly the class of bug the registry exists to stop.
  await patch({ disableLimiter: false });
  const limiterBefore = (await rest('/api/master/limiter')).body;
  await patch({ disableLimiter: 'please-turn-it-off' });
  const limiterAfter = (await rest('/api/master/limiter')).body;
  s = await settings();
  ok('a wrongly-typed disableLimiter is not stored', s.disableLimiter === false,
     String(s.disableLimiter));
  ok('and it did not reach the engine either',
     JSON.stringify(limiterBefore) === JSON.stringify(limiterAfter),
     `${JSON.stringify(limiterBefore)} -> ${JSON.stringify(limiterAfter)}`);

  // ---- 8. Legacy keys still accepted -----------------------------------
  // ltcDevice named a sound card in a document meant to travel; ltcOutput is
  // a logical output name resolved by the machine's output map (D38). The old
  // key is still ACCEPTED — a pre-2.5 client or a Companion button still sends
  // it — but it lands on the new one rather than being stored beside it, so
  // there is exactly one writer for where timecode goes (R1).
  await patch({ ltcOutput: 'Timecode' });
  s = await settings();
  ok('ltcOutput is stored', s.ltcOutput === 'Timecode', String(s.ltcOutput));

  await patch({ ltcDevice: 'Some Interface' });
  s = await settings();
  ok('a legacy ltcDevice patch still lands',
     s.ltcOutput === 'Some Interface', `ltcOutput = ${s.ltcOutput}`);
  ok('...on ltcOutput, and does not resurrect ltcDevice beside it',
     s.ltcDevice === undefined,
     s.ltcDevice === undefined ? '(absent)' : `ltcDevice = ${s.ltcDevice}`);

  await patch({ ltcOutput: null });
  s = await settings();
  ok('ltcOutput can be cleared with null', s.ltcOutput === null, String(s.ltcOutput));

  // ---- 9. outputTargetLevels: read yes, disk no -------------------------
  // The client depends on it being present on every read, so it must keep
  // arriving. It is derived, so it must not be written into the document —
  // it used to be, via the round trip, which put a stale copy on disk.
  await patch({ outputTarget: 'ebu-r128' });
  s = await settings();
  ok('outputTargetLevels is still present on a read',
     s.outputTargetLevels && typeof s.outputTargetLevels === 'object',
     JSON.stringify(s.outputTargetLevels ?? null)?.slice(0, 60));
  ok('outputTargetLevels tracks the chosen target',
     s.outputTargetLevels?.limiterCeilingDb === -1, String(s.outputTargetLevels?.limiterCeilingDb));

  // Round-trip the whole settings object back, exactly as the client's
  // watcher does — derived key and all — then save and read the file.
  const echo = await patch(s);
  ok('the whole settings object round-trips with 200', echo.status === 200, `status ${echo.status}`);

  const saved = await rest('/api/project/save', { method: 'POST', body: JSON.stringify({ path: file }) });
  ok('the project saved', saved.status === 200, `status ${saved.status}`);
  const onDisk = JSON.parse(fs.readFileSync(file, 'utf8'));
  const diskSettings = onDisk.settings ?? {};
  ok('outputTargetLevels is NOT written to the document',
     diskSettings.outputTargetLevels === undefined,
     JSON.stringify(diskSettings.outputTargetLevels ?? null));
  ok('unknown keys are NOT written to the document',
     diskSettings.notASetting === undefined && diskSettings.alsoNotOne === undefined,
     Object.keys(diskSettings).join(','));
  ok('the real settings did reach the document',
     diskSettings.outputTarget === 'ebu-r128' && diskSettings.stopAllFadeMs === 0,
     `outputTarget=${diskSettings.outputTarget} stopAllFadeMs=${diskSettings.stopAllFadeMs}`);

  try { fs.rmSync(dir, { recursive: true, force: true }); } catch {}

  console.log(`\n${failures === 0 ? 'ALL PASS' : 'FAILURES'} (${failures})`);
  process.exit(failures === 0 ? 0 : 1);
})().catch(e => { console.error('harness error:', e); process.exit(2); });

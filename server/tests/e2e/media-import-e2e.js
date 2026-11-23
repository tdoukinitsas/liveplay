// Two different files with one name must both survive an import (#92).
//
// Both routes a file can arrive by used to write straight to media/<name>, so
// the second import silently replaced the audio behind the first cue. What has
// to hold now, over the real HTTP routes:
//   * a different file with a taken name gets a numbered name;
//   * the bytes behind an earlier import never change;
//   * identical bytes reuse the existing copy instead of piling up;
//   * a file already in the media folder stays where it is;
//   * nothing half-written is left behind.
//
// usage: node media-import-e2e.js <port>
//
// Creates and removes its own temp tree; resets the project at the end.
const fs   = require('fs');
const os   = require('os');
const path = require('path');

const PORT = process.argv[2] || '4480';
const BASE = `http://127.0.0.1:${PORT}`;

let failures = 0;
const ok = (name, pass, detail = '') => {
  console.log(`${pass ? 'PASS' : 'FAIL'}  ${name}   ${detail}`);
  if (!pass) failures++;
};

async function rest(p, opts = {}) {
  const r = await fetch(BASE + p, { headers: { 'content-type': 'application/json' }, ...opts });
  const text = await r.text();
  let body; try { body = JSON.parse(text); } catch { body = text; }
  return { status: r.status, body };
}

const copyIn = async (source) => {
  const r = await rest('/api/copy_to_media', {
    method: 'POST', body: JSON.stringify({ source_path: source.replace(/\\/g, '/') }),
  });
  return r.body?.dest_path ?? `HTTP ${r.status}: ${JSON.stringify(r.body)}`;
};

const upload = async (name, bytes) => {
  const fd = new FormData();
  fd.append('file', new Blob([bytes]), name);
  const r = await fetch(`${BASE}/api/upload`, { method: 'POST', body: fd });
  const body = await r.json().catch(() => ({}));
  return body?.saved?.[0] ?? `HTTP ${r.status}`;
};

const base = p => String(p).split(/[\\/]/).pop();

(async () => {
  const root  = fs.mkdtempSync(path.join(os.tmpdir(), 'liveplay-media-import-'));
  const show  = path.join(root, 'Show');
  const media = path.join(show, 'media');
  const dirA  = path.join(root, 'from-a');
  const dirB  = path.join(root, 'from-b');

  try {
    fs.mkdirSync(media, { recursive: true });
    fs.mkdirSync(dirA);
    fs.mkdirSync(dirB);
    fs.writeFileSync(path.join(dirA, 'Intro.wav'), 'audio from folder A');
    fs.writeFileSync(path.join(dirB, 'Intro.wav'), 'audio from folder B');

    const put = await rest('/api/project/document', {
      method: 'PUT',
      body: JSON.stringify({
        name: 'mediaImportProbe', version: '2.0.0', busSchema: 2,
        folderPath: show.replace(/\\/g, '/'), items: [],
      }),
    });
    ok('the test project opens', put.status < 300, `HTTP ${put.status}`);

    const first = await copyIn(path.join(dirA, 'Intro.wav'));
    ok('the first file keeps its name', base(first) === 'Intro.wav', first);

    const second = await copyIn(path.join(dirB, 'Intro.wav'));
    ok('a different file gets a numbered name', base(second) === 'Intro (2).wav', second);
    ok('the first cue\'s audio is unchanged (#92)',
       fs.readFileSync(path.join(media, 'Intro.wav'), 'utf8') === 'audio from folder A');
    ok('the second file\'s audio landed',
       fs.readFileSync(path.join(media, 'Intro (2).wav'), 'utf8') === 'audio from folder B');

    const repeat = await copyIn(path.join(dirA, 'Intro.wav'));
    ok('re-importing identical bytes reuses the copy', base(repeat) === 'Intro.wav', repeat);

    const inPlace = await copyIn(path.join(media, 'Intro (2).wav'));
    ok('a file already in media stays put', base(inPlace) === 'Intro (2).wav', inPlace);

    const third = await upload('Intro.wav', 'audio uploaded from a remote client');
    ok('an upload cannot overwrite either', base(third) === 'Intro (3).wav', third);
    ok('the first cue\'s audio survives the upload',
       fs.readFileSync(path.join(media, 'Intro.wav'), 'utf8') === 'audio from folder A');

    const reupload = await upload('Intro.wav', 'audio from folder B');
    ok('an identical upload reuses the copy', base(reupload) === 'Intro (2).wav', reupload);

    // Two clients dropping a same-named file at the same moment.
    const racers = await Promise.all(
      Array.from({ length: 8 }, (_, i) => upload('Race.wav', `racer ${i}`)));
    ok('simultaneous imports each get their own file', new Set(racers).size === 8,
       racers.map(base).join(', '));
    const intact = racers.every(p => /^racer \d$/.test(fs.readFileSync(p, 'utf8')));
    const distinct = new Set(racers.map(p => fs.readFileSync(p, 'utf8'))).size === 8;
    ok('and none was overwritten', intact && distinct);

    const left = fs.readdirSync(media).filter(f => f.endsWith('.tmp'));
    ok('no temp files are left behind', left.length === 0, left.join(', '));
    ok('the folder holds exactly the expected files',
       fs.readdirSync(media).length === 3 + 8, String(fs.readdirSync(media).length));
  } catch (e) {
    ok('the suite ran to the end', false, e?.stack ?? String(e));
  } finally {
    await rest('/api/project/close', { method: 'POST' }).catch(() => {});
    try { fs.rmSync(root, { recursive: true, force: true }); } catch { /* best effort */ }
  }

  console.log(failures ? `\n${failures} check(s) failed` : '\nAll checks passed');
  process.exit(failures ? 1 : 0);
})();

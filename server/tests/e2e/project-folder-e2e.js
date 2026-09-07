// A show file must not name a directory on the machine that saved it.
//
// Every .liveplay carried `folderPath`: an absolute path to wherever the
// project happened to live when it was last written. It is a fact about a
// machine, stored in the one file that travels between them — mail the project
// to a colleague, unzip a .lpa somewhere else, or just move the folder, and the
// value is wrong while the file's own location is right. load() has always
// overwritten it on the way in for exactly that reason, which is what makes
// dropping it safe: nothing read what was being written.
//
// So the field is now derived, not stored. What that has to mean:
//   • the saved file does not contain it at all;
//   • opening a project that has MOVED still finds its media, because the
//     folder is read off the file's location;
//   • a Save As re-anchors the folder (and media_root with it) on the way out,
//     rather than leaving the server pointing at the folder just saved away
//     from — which is where the next import used to land;
//   • a file written before 2.5, which still carries the field, loads and is
//     corrected rather than believed.
//
// usage: node project-folder-e2e.js <port> <wavPath>
//
// Creates and removes its own temp tree; closes the project at the end.
const fs   = require('fs');
const os   = require('os');
const path = require('path');

const PORT = process.argv[2] || '4480';
const WAV  = process.argv[3];
const BASE = `http://127.0.0.1:${PORT}`;

let failures = 0;
const ok = (name, pass, detail) => {
  if (detail === undefined) detail = '(no detail given — fix the assertion)';
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

// The server reports native paths (backslashes on Windows) and the client sends
// forward slashes; neither difference is what this test is about.
const samePath = (a, b) => {
  const norm = p => {
    const s = String(p ?? '').replace(/[\\/]+/g, '/').replace(/\/+$/, '');
    return process.platform === 'win32' ? s.toLowerCase() : s;
  };
  return norm(a) === norm(b);
};

const uuid = 'item-folder-0001';
const doc  = async () => (await rest('/api/project')).body;

// The engine holds a decoder open on every loaded cue, so a project folder
// cannot be renamed out from under it on Windows. Close first, then retry:
// handles are released on the mirror thread, not synchronously with the reply.
async function moveFolder(from, to) {
  for (let i = 0; i < 20; i++) {
    try { fs.renameSync(from, to); return true; } catch { await sleep(200); }
  }
  return false;
}

async function waitLoaded() {
  for (let i = 0; i < 80; i++) {
    const p = await rest('/api/project/progress');
    if (p.body && p.body.loading === false) return true;
    await sleep(100);
  }
  return false;
}

(async () => {
  if (!WAV || !fs.existsSync(WAV)) {
    console.log(`FAIL  the signal wav must exist   '${WAV}'`);
    process.exit(1);
  }
  const root  = fs.mkdtempSync(path.join(os.tmpdir(), 'liveplay-folder-'));
  const home  = path.join(root, 'HomeShow');
  const venue = path.join(root, 'VenueShow');
  const copy  = path.join(root, 'CopyShow');
  const relic = path.join(root, 'RelicShow');

  try {
    fs.mkdirSync(path.join(home, 'media'), { recursive: true });
    fs.copyFileSync(WAV, path.join(home, 'media', 'probe.wav'));

    // ---- A project saved from its own folder ---------------------------
    await rest('/api/project/document', {
      method: 'PUT',
      body: JSON.stringify({
        name: 'folderProbe', version: '2.0.0', busSchema: 2,
        folderPath: home.replace(/\\/g, '/'),
        items: [{ uuid, type: 'audio', displayName: 'Probe',
                  mediaServerPath: path.join(home, 'media', 'probe.wav').replace(/\\/g, '/'),
                  volume: 1, endBehavior: 'stop' }],
      }),
    });
    await waitLoaded();

    const homeFile = path.join(home, 'Show.liveplay');
    let r = await rest('/api/project/save', {
      method: 'POST', body: JSON.stringify({ path: homeFile.replace(/\\/g, '/') }),
    });
    ok('the project saves', r.status === 200, `status ${r.status}`);

    const saved = JSON.parse(fs.readFileSync(homeFile, 'utf8'));
    ok('the saved file names no folder on this machine',
       !Object.prototype.hasOwnProperty.call(saved, 'folderPath'),
       saved.folderPath === undefined ? '(absent)' : `folderPath = ${saved.folderPath}`);
    ok('the saved file still carries the project',
       saved.name === 'folderProbe' && Array.isArray(saved.items) && saved.items.length === 1,
       `name '${saved.name}', ${(saved.items || []).length} item(s)`);
    ok('its media is stored relative to the folder it no longer names',
       saved.items[0].mediaPath === 'media/probe.wav',
       `mediaPath = ${saved.items[0].mediaPath}`);

    // ---- The whole project moves ---------------------------------------
    await rest('/api/project/close', { method: 'POST', body: '{}' });
    await sleep(400);
    const moved = await moveFolder(home, venue);
    ok('the project folder can be moved once the project is closed', moved,
       moved ? `${path.basename(home)} -> ${path.basename(venue)}`
             : 'rename never succeeded — the harness could not stage the move');
    if (!moved) throw new Error('cannot continue without the move');

    const venueFile = path.join(venue, 'Show.liveplay');
    r = await rest('/api/project/load', {
      method: 'POST', body: JSON.stringify({ path: venueFile.replace(/\\/g, '/') }),
    });
    ok('the moved project opens', r.status === 200, `status ${r.status}`);
    await waitLoaded();

    let d = await doc();
    ok('it reports the folder it is actually in, not the one it was saved from',
       samePath(d.folderPath, venue), `folderPath = ${d.folderPath}`);
    ok('media root follows the file',
       samePath(d.server && d.server.mediaRoot, path.join(venue, 'media')),
       `mediaRoot = ${d.server && d.server.mediaRoot}`);

    // The claim that matters: not that a string was rewritten, but that the
    // audio still resolves through it. A cue that failed to load has no cueId.
    const item = (d.items || []).find(i => i.uuid === uuid);
    ok('the cue still loads after the move',
       !!item && typeof item.cueId === 'string' && item.cueId.length > 0,
       item ? `cueId '${item.cueId}'` : '(item missing)');
    const cues = (await rest('/api/cues')).body;
    const cueFile = Array.isArray(cues) && cues[0] ? (cues[0].file_path || '') : '';
    ok('and it resolves inside the new folder',
       samePath(path.dirname(cueFile), path.join(venue, 'media')),
       `file_path = ${cueFile || '(none)'}`);

    // ---- Save As re-anchors ---------------------------------------------
    fs.mkdirSync(copy, { recursive: true });
    const copyFile = path.join(copy, 'Show.liveplay');
    r = await rest('/api/project/save', {
      method: 'POST', body: JSON.stringify({ path: copyFile.replace(/\\/g, '/') }),
    });
    ok('a save-as writes', r.status === 200, `status ${r.status}`);
    d = await doc();
    ok('a save-as moves the project folder to where it just wrote',
       samePath(d.folderPath, copy), `folderPath = ${d.folderPath}`);
    ok('...and takes media root with it, so the next import lands in the copy',
       samePath(d.server && d.server.mediaRoot, path.join(copy, 'media')),
       `mediaRoot = ${d.server && d.server.mediaRoot}`);
    const copied = JSON.parse(fs.readFileSync(copyFile, 'utf8'));
    ok('the copy names no folder either',
       !Object.prototype.hasOwnProperty.call(copied, 'folderPath'),
       copied.folderPath === undefined ? '(absent)' : `folderPath = ${copied.folderPath}`);
    ok('and still carries the project it copied',
       copied.name === 'folderProbe' && (copied.items || []).length === 1,
       `name '${copied.name}', ${(copied.items || []).length} item(s)`);
    // NOT asserted here: that the copy's media still resolves. A save-as does
    // not copy the media folder, so the copy points at a "media/probe.wav" that
    // does not exist beside it — true before this change too (the copy then
    // carried the OLD folderPath, which load() overwrote on the way in, landing
    // in the same place by a different route). Pinning it either way would
    // enshrine the current answer to a question nobody has decided: whether a
    // save-as should copy media, re-point at the original, or refuse.

    // ---- A file from before the field was dropped ------------------------
    fs.mkdirSync(path.join(relic, 'media'), { recursive: true });
    fs.copyFileSync(WAV, path.join(relic, 'media', 'probe.wav'));
    const relicFile = path.join(relic, 'Show.liveplay');
    fs.writeFileSync(relicFile, JSON.stringify({
      name: 'relic', version: '2.0.0', busSchema: 2,
      folderPath: '/somewhere/on/another/machine',
      items: [{ uuid, type: 'audio', displayName: 'Probe',
                mediaPath: 'media/probe.wav', volume: 1, endBehavior: 'stop' }],
    }, null, 2));
    r = await rest('/api/project/load', {
      method: 'POST', body: JSON.stringify({ path: relicFile.replace(/\\/g, '/') }),
    });
    ok('a project saved before 2.5 still opens', r.status === 200, `status ${r.status}`);
    await waitLoaded();
    d = await doc();
    ok('the folder it names is corrected, not believed',
       samePath(d.folderPath, relic), `folderPath = ${d.folderPath}`);
    const relicItem = (d.items || []).find(i => i.uuid === uuid);
    ok('so its media resolves here',
       !!relicItem && typeof relicItem.cueId === 'string' && relicItem.cueId.length > 0,
       relicItem ? `cueId '${relicItem.cueId}'` : '(item missing)');

    await rest('/api/project/close', { method: 'POST', body: '{}' });
    await sleep(400);
  } finally {
    try { fs.rmSync(root, { recursive: true, force: true, maxRetries: 10 }); } catch {}
  }

  console.log(`\n${failures === 0 ? 'ALL PASS' : 'FAILURES'} (${failures})`);
  process.exit(failures === 0 ? 0 : 1);
})();

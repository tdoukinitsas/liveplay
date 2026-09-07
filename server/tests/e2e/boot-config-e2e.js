// liveplay.json — the boot configuration file, and the precedence around it.
//
// Everything the server can be told at boot could only be told to it on the
// command line or through the environment, which means an installation's
// posture — which port, which origin, which part of the filesystem the API may
// reach — lived on a shortcut. Shortcuts do not survive a reinstall, and the
// crash handler relaunches from argv, not from anyone's intent.
//
// So: a sparse JSON file beside the executable. Sparse matters. An absent key
// means "not set", never "the default", so a file written today keeps taking
// improved defaults instead of freezing this version's — and the server never
// writes the file, because nothing it wrote could tell a deliberate choice
// apart from a default that happened to be current.
//
// What this pins:
//   • a file alone configures the server — no flags at all;
//   • precedence really is default < file < environment < command line;
//   • --fs-root REPLACES the file's roots rather than extending them, or a
//     flag could only ever widen an installation's jail and never narrow it;
//   • one bad key costs that key and nothing else — the server still boots,
//     and the good keys in the same file still land;
//   • the problems are SAID. A silently ignored config file is worse than no
//     config file, because the operator believes the posture is set.
//
// Starts its own servers (it is testing boot configuration), so it takes the
// binary rather than a port.
//
//   node server/tests/e2e/boot-config-e2e.js [path-to-liveplay-server]
const { spawn, spawnSync } = require('child_process');
const fs   = require('fs');
const os   = require('os');
const path = require('path');

const EXE = process.argv[2] || path.join('server', 'build', 'Release',
  process.platform === 'win32' ? 'liveplay-server.exe' : 'liveplay-server');

const FILE_PORT = 4561;   // configured by the file alone
const FLAG_PORT = 4562;   // where the flag says, not where the file says
const JUNK_PORT = 4563;   // the file is full of mistakes
const ENV_PORT  = 4564;   // environment over file

let failures = 0;
const ok = (n, p, d = '') => { console.log(`${p ? 'PASS' : 'FAIL'}  ${n}${d ? '   ' + d : ''}`); if (!p) failures++; };

// A server that never came up is a FAILED ASSERTION, not a crashed harness:
// half the point of this suite is what happens when configuration is wrong, and
// a run that aborts on the first unreachable port reports one line instead of
// the several that would say which tier broke. Status 0 means "no answer".
const rest = async (port, p, o = {}) => {
  try {
    const r = await fetch(`http://127.0.0.1:${port}${p}`,
      { headers: { 'content-type': 'application/json' }, ...o });
    const t = await r.text();
    let body; try { body = JSON.parse(t); } catch { body = t; }
    return { status: r.status, body, cors: r.headers.get('access-control-allow-origin') };
  } catch (e) {
    return { status: 0, body: null, cors: null, unreachable: String(e.cause || e) };
  }
};
const sleep = ms => new Promise(r => setTimeout(r, ms));

async function waitForHealth(port, tries = 40) {
  for (let i = 0; i < tries; i++) {
    try {
      const r = await fetch(`http://127.0.0.1:${port}/api/health`);
      if (r.ok) return true;
    } catch { /* not up yet */ }
    await sleep(250);
  }
  return false;
}
async function stillDown(port) {
  try { const r = await fetch(`http://127.0.0.1:${port}/api/health`); return !r.ok; }
  catch { return true; }
}

// --help exits before binding anything, so it is a cheap way to see what the
// config reader SAID about a file without paying for a whole server.
const readsAs = (configPath) => {
  const r = spawnSync(EXE, ['--config', configPath, '--help'], { encoding: 'utf8' });
  return { code: r.status, out: `${r.stdout || ''}${r.stderr || ''}` };
};
// The help text is in the same capture, so a detail line has to be pulled by
// the same pattern the assertion used — matching on the key name alone finds
// the documentation table instead of the complaint.
const lineWith = (out, re) => {
  const hit = out.split(/\r?\n/).find(l => re.test(l));
  return hit ? hit.trim() : '(silent)';
};

(async () => {
  const root    = fs.mkdtempSync(path.join(os.tmpdir(), 'liveplay-cfg-'));
  const allowed = path.join(root, 'shows');       // named by the config file
  const other   = path.join(root, 'elsewhere');   // named by the flag instead
  fs.mkdirSync(allowed);
  fs.mkdirSync(other);
  const write = (name, text) => {
    const p = path.join(root, name);
    fs.writeFileSync(p, text);
    return p;
  };

  const good = write('good.json', JSON.stringify({
    schema_version: 1,
    port: FILE_PORT,
    bind: '127.0.0.1',
    corsOrigin: 'https://booth.example',
    fsRoots: [allowed],
  }, null, 2));

  // One good key among four mistakes: an unknown key, a string where a number
  // belongs, a number outside its range, and an fsRoots that is not an array.
  const junk = write('junk.json', JSON.stringify({
    schema_version: 1,
    corsOrigin: 'https://survivor.example',
    nonsense: true,
    port: '4599',
    meterHz: 9999,
    fsRoots: 'C:\\',
  }, null, 2));

  const malformed = write('malformed.json', '{ this is not json');
  const missing   = path.join(root, 'no-such-config.json');

  const newer = write('newer.json', JSON.stringify({
    schema_version: 99, corsOrigin: 'https://future.example',
  }, null, 2));

  const procs = [];
  const start = (args, env) => {
    const p = spawn(EXE, args, { stdio: 'ignore', env: { ...process.env, ...(env || {}) } });
    procs.push(p);
    return p;
  };

  try {
    // No --port, no --bind, no --cors-origin, no --fs-root: the file is the
    // only thing that configures this one.
    start(['--config', good]);
    // The same file, overruled on every point it makes.
    start(['--config', good, '--port', String(FLAG_PORT), '--bind', '127.0.0.1',
           '--cors-origin', 'https://flag.example', '--fs-root', other]);
    start(['--config', junk, '--port', String(JUNK_PORT), '--bind', '127.0.0.1']);
    start(['--config', good, '--port', String(ENV_PORT), '--bind', '127.0.0.1'],
          { LIVEPLAY_CORS_ORIGIN: 'https://env.example' });

    // ---- The file alone configures the server ---------------------------
    ok('a config file with no flags brings the server up on its port',
       await waitForHealth(FILE_PORT), `port ${FILE_PORT}`);
    let r = await rest(FILE_PORT, '/api/health');
    ok('the origin it names is sent', r.cors === 'https://booth.example', String(r.cors));
    r = await rest(FILE_PORT, `/api/fs/list?path=${encodeURIComponent(allowed)}&filter=all`);
    ok('the root it names is reachable', r.status === 200, `status ${r.status}`);
    r = await rest(FILE_PORT, `/api/fs/list?path=${encodeURIComponent(os.homedir())}&filter=all`);
    ok('and everything else is not', r.status === 403, `status ${r.status}`);

    // ---- A flag overrules the file --------------------------------------
    ok('a --port beats the file', await waitForHealth(FLAG_PORT), `port ${FLAG_PORT}`);
    r = await rest(FLAG_PORT, '/api/health');
    ok('a --cors-origin beats the file', r.cors === 'https://flag.example', String(r.cors));
    r = await rest(FLAG_PORT, `/api/fs/list?path=${encodeURIComponent(other)}&filter=all`);
    ok('the root named on the command line is reachable', r.status === 200, `status ${r.status}`);
    // The half that matters: not that the flag's root works, but that the
    // file's no longer does. Appending would leave an installation unable to
    // narrow its own jail from the command line.
    r = await rest(FLAG_PORT, `/api/fs/list?path=${encodeURIComponent(allowed)}&filter=all`);
    ok('--fs-root REPLACES the file\'s roots rather than adding to them',
       r.status === 403, `the file's root -> ${r.status}`);

    // ---- The environment sits between them ------------------------------
    ok('a server with an env override comes up', await waitForHealth(ENV_PORT), `port ${ENV_PORT}`);
    r = await rest(ENV_PORT, '/api/health');
    ok('the environment beats the file', r.cors === 'https://env.example', String(r.cors));

    // ---- One bad key costs one key --------------------------------------
    ok('a config full of mistakes still boots the server',
       await waitForHealth(JUNK_PORT), `port ${JUNK_PORT}`);
    r = await rest(JUNK_PORT, '/api/health');
    ok('and the one good key in it still landed',
       r.cors === 'https://survivor.example', String(r.cors));
    r = await rest(JUNK_PORT, `/api/fs/list?path=${encodeURIComponent(os.homedir())}&filter=all`);
    ok('an fsRoots of the wrong type leaves the API unrestricted, not confined to nothing',
       r.status === 200, `status ${r.status}`);
    ok('a quoted port does not take effect', await stillDown(4599), 'port 4599');

    // ---- Every problem is said out loud ---------------------------------
    let h = readsAs(junk);
    let re = /unknown key 'nonsense'/;
    ok('an unknown key is named', re.test(h.out), lineWith(h.out, re));
    re = /'port' must be a whole number/;
    ok('a wrong type is named', re.test(h.out), lineWith(h.out, re));
    re = /meterHz = 9999 is outside the supported range \[1, 120\]/;
    ok('an out-of-range value is named and its range given',
       re.test(h.out), lineWith(h.out, re));

    h = readsAs(malformed);
    re = /not a valid JSON object/;
    ok('a malformed file is reported rather than half-read',
       re.test(h.out), lineWith(h.out, re));
    ok('...and the server still starts', h.code === 0, `exit ${h.code}`);

    h = readsAs(missing);
    re = /does not exist/;
    ok('a named config that is not there is reported as the typo it usually is',
       re.test(h.out), lineWith(h.out, re));
    ok('...and the server still starts', h.code === 0, `exit ${h.code}`);

    h = readsAs(newer);
    re = /schema_version 99/;
    ok('a file from a newer schema is read for what this server understands',
       re.test(h.out), lineWith(h.out, re));

    // The keys --help lists come from the same table the reader validates
    // against, so this also pins that a readable key cannot go undocumented.
    h = readsAs(good);
    ok('--help documents the keys and the precedence',
       /Config keys \(schema_version 1\)/.test(h.out) &&
       /corsOrigin/.test(h.out) && /fsRoots/.test(h.out) &&
       /built-in default  <  liveplay.json  <  environment  <  command line/.test(h.out),
       (h.out.match(/Config keys[^\n]*/) || ['(missing)'])[0].trim());
  } finally {
    for (const p of procs) { try { p.kill(); } catch {} }
    await sleep(300);
    try { fs.rmSync(root, { recursive: true, force: true, maxRetries: 10 }); } catch {}
  }

  console.log(`\n${failures === 0 ? 'ALL PASS' : 'FAILURES'} (${failures})`);
  process.exit(failures === 0 ? 0 : 1);
})().catch(e => { console.error('harness error:', e); process.exit(2); });

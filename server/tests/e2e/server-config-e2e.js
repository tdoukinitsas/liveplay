// The machine's own settings, editable from the settings page (P3a).
//
// S3 gave liveplay.json a reader and said out loud that the server would never
// write it. P3 gives it a writer, which is only not a contradiction because of
// how the writer behaves: it touches ONLY the keys a request names, so the file
// stays sparse and an installation keeps taking improved defaults for
// everything nobody has chosen. Most of this suite is about that, and about the
// page not being able to lie.
//
// What this pins:
//   • SPARSE STAYS SPARSE. Patching one key writes one key. A file with two
//     keys in it does not become a file with thirteen.
//   • Null CLEARS a key, which is how "stop pinning this, go back to the
//     built-in default" is said. Without it a value could be changed but never
//     un-chosen, and the sparse file would fill up one edit at a time.
//   • THE PAGE CANNOT LIE ABOUT PROVENANCE. A value supplied by a flag or the
//     environment reports `source` and `overridden`, because the desktop app
//     always launches with --port: a port field that accepted an edit and said
//     nothing would write the file, report success, and change nothing.
//   • ...and it still writes. Being overridden does not make the file
//     read-only — the stored value is what applies once the flag goes away —
//     so the response says which keys were written but are not in force, and
//     distinguishes "waiting for a restart" from "shadowed by a flag", which
//     are different facts with different fixes.
//   • THE LOCK IS A LOCK (R3). --lock-server-config refuses every write with
//     403, including from an administrator, and cannot be turned off through
//     the API — a lock an admin can pick over the network is not one.
//   • Validation drops key by key rather than failing the patch, matching the
//     project settings registry, and out-of-range values cannot get in by the
//     API door that the file door would have refused.
//   • It is ADMIN-ONLY. An operator is refused; this is the Server tier, and
//     fsRoots in particular decides how much of the filesystem the API can
//     reach.
//
// Starts its own servers (it is testing boot-time provenance and a lock), and
// OWNS liveplay.json and users.json beside the binary for the duration — both
// are moved aside and put back.
//
//   node server/tests/e2e/server-config-e2e.js [path-to-liveplay-server]
const { spawn } = require('child_process');
const path = require('path');
const fs   = require('fs');

const EXE = process.argv[2] || path.join('server', 'build', 'Release',
  process.platform === 'win32' ? 'liveplay-server.exe' : 'liveplay-server');

const PORT       = 4575;
const EXE_DIR    = path.dirname(path.resolve(EXE));
const CONFIG     = path.join(EXE_DIR, 'liveplay.json');
const USERS_JSON = path.join(EXE_DIR, 'users.json');
const PREFS_DIR  = path.join(EXE_DIR, 'prefs');
const C_BACKUP   = CONFIG     + '.e2e-backup';
const U_BACKUP   = USERS_JSON + '.e2e-backup';
const P_BACKUP   = PREFS_DIR  + '.e2e-backup';

const ADMIN_NAME = 'e2e-cfg-admin';
const ADMIN_PASS = 'correct-horse-battery';
const OP_NAME    = 'e2e-cfg-operator';
const OP_PASS    = 'stapler-in-the-jelly';

let failures = 0;
const ok = (n, p, d) => {
  if (d === undefined) d = '(no detail given — fix the assertion)';
  console.log(`${p ? 'PASS' : 'FAIL'}  ${n}   ${d}`);
  if (!p) failures++;
};
const sleep = ms => new Promise(r => setTimeout(r, ms));

async function req(p, { method = 'GET', token, body } = {}) {
  const headers = {};
  if (token) headers.Authorization = `Bearer ${token}`;
  if (body !== undefined) headers['Content-Type'] = 'application/json';
  try {
    const r = await fetch(`http://127.0.0.1:${PORT}${p}`, {
      method, headers,
      body: body === undefined ? undefined : JSON.stringify(body),
    });
    const t = await r.text();
    let parsed; try { parsed = JSON.parse(t); } catch { parsed = t; }
    return { status: r.status, body: parsed };
  } catch (e) {
    return { status: 0, body: null, unreachable: String(e.cause || e) };
  }
}

async function waitForHealth(tries = 40) {
  for (let i = 0; i < tries; i++) {
    try { const r = await fetch(`http://127.0.0.1:${PORT}/api/health`); if (r.ok) return true; }
    catch { /* not up yet */ }
    await sleep(250);
  }
  return false;
}

const login = async (name, password) => {
  const r = await req('/api/auth/login', { method: 'POST', body: { name, password } });
  return (r.body && r.body.token) || '';
};

const readConfig = () => {
  try { return JSON.parse(fs.readFileSync(CONFIG, 'utf8')); } catch { return null; }
};
const fieldOf = (body, key) =>
  (body && Array.isArray(body.fields) ? body.fields.find(f => f.key === key) : null) || {};

(async () => {
  let proc = null;
  const startServer = (extraArgs = [], env = {}) => {
    proc = spawn(EXE, ['--port', String(PORT), ...extraArgs],
                 { stdio: 'ignore', env: { ...process.env, ...env } });
    return proc;
  };
  const stopServer = async () => {
    if (!proc) return;
    try { proc.kill(); } catch { /* already gone */ }
    proc = null;
    await sleep(1200);
  };

  let hadCfg = false, hadUsers = false, hadPrefs = false;
  try {
    if (fs.existsSync(CONFIG))     { fs.renameSync(CONFIG, C_BACKUP);     hadCfg = true; }
    if (fs.existsSync(USERS_JSON)) { fs.renameSync(USERS_JSON, U_BACKUP); hadUsers = true; }
    if (fs.existsSync(PREFS_DIR))  { fs.renameSync(PREFS_DIR, P_BACKUP);  hadPrefs = true; }
  } catch (e) {
    console.log(`FAIL  could not move existing state aside   ${e}`);
    process.exit(1);
  }

  let adminToken = '', opToken = '';

  try {
    // =====================================================================
    // 1. No accounts yet — the route is reachable, because nobody is an admin
    // =====================================================================
    startServer();
    ok('the server starts with no config file', await waitForHealth(), `port ${PORT}`);
    ok('...and wrote no liveplay.json just by starting',
       !fs.existsSync(CONFIG), CONFIG);

    let r = await req('/api/server/config');
    ok('GET /api/server/config describes every key in the schema',
       r.status === 200 && Array.isArray(r.body.fields) && r.body.fields.length >= 13,
       `${r.status}, ${r.body && r.body.fields && r.body.fields.length} field(s)`);
    ok('...and reports the file it would write, so the page can name it',
       typeof r.body.path === 'string' && r.body.path.endsWith('liveplay.json'),
       String(r.body && r.body.path));
    ok('...and that nothing is locked by default, matching every other posture knob',
       r.body.locked === false, `locked=${r.body && r.body.locked}`);

    // The desktop app always launches with --port, and so does this suite.
    // This is the assertion that keeps the settings page honest.
    ok('a value supplied by a command-line flag says so',
       fieldOf(r.body, 'port').source === 'cli',
       `port source=${fieldOf(r.body, 'port').source}`);
    ok('...and is marked as overridden, so the page greys it rather than lying',
       fieldOf(r.body, 'port').overridden === true,
       `overridden=${fieldOf(r.body, 'port').overridden}`);
    ok('a value nobody set reports the built-in default as its source',
       fieldOf(r.body, 'corsOrigin').source === 'default' &&
       fieldOf(r.body, 'corsOrigin').overridden === false,
       `corsOrigin source=${fieldOf(r.body, 'corsOrigin').source}`);
    ok('...and still reports what is in force, so the form is never blank',
       fieldOf(r.body, 'corsOrigin').value === '*',
       `corsOrigin value=${JSON.stringify(fieldOf(r.body, 'corsOrigin').value)}`);
    ok('the two policy keys are marked as such — the lock exists for them',
       fieldOf(r.body, 'fsRoots').policy === true &&
       fieldOf(r.body, 'corsOrigin').policy === true &&
       fieldOf(r.body, 'meterHz').policy === false,
       `fsRoots=${fieldOf(r.body, 'fsRoots').policy} meterHz=${fieldOf(r.body, 'meterHz').policy}`);
    ok('every field says when a change takes effect',
       r.body.fields.every(f => f.appliesAt === 'restart' || f.appliesAt === 'live'),
       `distinct: ${[...new Set(r.body.fields.map(f => f.appliesAt))].join(',')}`);
    ok('numeric fields carry their range, so the page validates the same way the server does',
       fieldOf(r.body, 'meterHz').min === 1 && fieldOf(r.body, 'meterHz').max === 120,
       `meterHz ${fieldOf(r.body, 'meterHz').min}..${fieldOf(r.body, 'meterHz').max}`);

    // =====================================================================
    // 2. Sparse stays sparse
    // =====================================================================
    r = await req('/api/server/config', {
      method: 'PATCH', body: { corsOrigin: 'https://console.example' },
    });
    ok('a patch of one key succeeds', r.status === 200, `${r.status}`);
    let file = readConfig();
    ok('...and writes exactly that key, not the whole configuration',
       file && Object.keys(file).filter(k => k !== 'schema_version').length === 1 &&
       file.corsOrigin === 'https://console.example',
       JSON.stringify(file));
    ok('...stamped with the schema version, so an older server knows what it is reading',
       file && file.schema_version === 1, `schema_version=${file && file.schema_version}`);

    r = await req('/api/server/config', { method: 'PATCH', body: { meterHz: 15 } });
    file = readConfig();
    ok('a second patch adds its key and leaves the first alone',
       file && file.meterHz === 15 && file.corsOrigin === 'https://console.example',
       JSON.stringify(file));

    // The one that matters for sparseness: without this, a value could be
    // changed but never un-chosen, and the file would fill up one edit at a
    // time until every default was frozen.
    r = await req('/api/server/config', { method: 'PATCH', body: { meterHz: null } });
    file = readConfig();
    ok('a null CLEARS a key rather than storing null',
       r.status === 200 && file && !('meterHz' in file),
       JSON.stringify(file));

    // =====================================================================
    // 3. Validation — dropped key by key, same rules as the file door
    // =====================================================================
    r = await req('/api/server/config', {
      method: 'PATCH',
      body: {
        meterHz: 9999,            // out of range
        port: 'not-a-number',     // wrong type
        corsOrigin: '',           // empty means unset, and unset is null
        notASetting: 'whatever',  // unknown
        maxUploadMb: 64,          // ...and one good one
      },
    });
    ok('a patch of mostly-invalid values still returns 200 rather than failing the form',
       r.status === 200, `${r.status}`);
    ok('...and says what it dropped, so the page can show it against the right fields',
       Array.isArray(r.body.dropped) && r.body.dropped.length === 4,
       JSON.stringify(r.body && r.body.dropped));
    file = readConfig();
    ok('...none of the bad values were stored',
       file && file.meterHz === undefined && file.port === undefined &&
       file.notASetting === undefined,
       JSON.stringify(file));
    ok('...the earlier corsOrigin survived an empty-string patch rather than being cleared',
       file && file.corsOrigin === 'https://console.example',
       `corsOrigin=${file && file.corsOrigin}`);
    ok('...and the one good key in the same patch landed',
       file && file.maxUploadMb === 64, `maxUploadMb=${file && file.maxUploadMb}`);

    // =====================================================================
    // 4. Written, but not in force — and the two reasons are different
    // =====================================================================
    r = await req('/api/server/config', {
      method: 'PATCH', body: { port: 4999, maxUploadMb: 32 },
    });
    ok('a key the command line is supplying is still WRITTEN — the file is what applies later',
       r.status === 200 && readConfig().port === 4999,
       `stored port=${readConfig().port}`);
    ok('...but is reported as overridden at launch, not as pending a restart',
       Array.isArray(r.body.overriddenAtLaunch) &&
       r.body.overriddenAtLaunch.includes('port') &&
       !(r.body.restartRequired || []).includes('port'),
       `overridden=${JSON.stringify(r.body.overriddenAtLaunch)} ` +
       `restart=${JSON.stringify(r.body.restartRequired)}`);
    ok('...while a key nobody is overriding is reported as pending a restart',
       (r.body.restartRequired || []).includes('maxUploadMb'),
       `restart=${JSON.stringify(r.body.restartRequired)}`);

    // =====================================================================
    // 5. It survives a restart, and provenance flips to the file
    // =====================================================================
    await req('/api/server/config', { method: 'PATCH', body: { port: null } });
    await stopServer();
    startServer();
    ok('the server restarts, reading what was written', await waitForHealth(), `port ${PORT}`);

    r = await req('/api/server/config');
    ok('a value written through the API is in force after a restart',
       fieldOf(r.body, 'corsOrigin').value === 'https://console.example',
       `corsOrigin=${JSON.stringify(fieldOf(r.body, 'corsOrigin').value)}`);
    ok('...and now reports the FILE as its source, not a flag or a default',
       fieldOf(r.body, 'corsOrigin').source === 'file' &&
       fieldOf(r.body, 'corsOrigin').overridden === false,
       `source=${fieldOf(r.body, 'corsOrigin').source}`);
    ok('...and the page can show what is stored separately from what is in force',
       fieldOf(r.body, 'corsOrigin').stored === 'https://console.example',
       `stored=${JSON.stringify(fieldOf(r.body, 'corsOrigin').stored)}`);

    // =====================================================================
    // 6. The environment tier still reports itself
    // =====================================================================
    await stopServer();
    startServer([], { LIVEPLAY_CORS_ORIGIN: 'https://env.example' });
    ok('the server restarts with an environment override', await waitForHealth(), `port ${PORT}`);
    r = await req('/api/server/config');
    ok('an environment variable beats the file, and says so',
       fieldOf(r.body, 'corsOrigin').source === 'env' &&
       fieldOf(r.body, 'corsOrigin').value === 'https://env.example',
       `source=${fieldOf(r.body, 'corsOrigin').source} ` +
       `value=${JSON.stringify(fieldOf(r.body, 'corsOrigin').value)}`);
    ok('...while still showing the file value underneath it, so the page can explain the conflict',
       fieldOf(r.body, 'corsOrigin').stored === 'https://console.example',
       `stored=${JSON.stringify(fieldOf(r.body, 'corsOrigin').stored)}`);

    // =====================================================================
    // 7. The lock (R3)
    // =====================================================================
    await stopServer();
    startServer(['--lock-server-config']);
    ok('the server restarts with the configuration locked', await waitForHealth(), `port ${PORT}`);

    r = await req('/api/server/config');
    ok('the lock is reported, so the page renders read-only instead of failing on save',
       r.body && r.body.locked === true, `locked=${r.body && r.body.locked}`);

    const beforeLock = JSON.stringify(readConfig());
    r = await req('/api/server/config', { method: 'PATCH', body: { meterHz: 42 } });
    ok('a write is refused with 403 while locked',
       r.status === 403, `${r.status} ${JSON.stringify(r.body)}`);
    ok('...and the file is untouched',
       JSON.stringify(readConfig()) === beforeLock,
       `before ${beforeLock} / after ${JSON.stringify(readConfig())}`);

    // The whole point: a lock that can be turned off through the thing it
    // locks is not a lock.
    r = await req('/api/server/config', {
      method: 'PATCH', body: { lockServerConfig: false },
    });
    ok('the lock cannot be turned off through the API it locks',
       r.status === 403, `${r.status}`);

    // =====================================================================
    // 8. Server tier — administrators only
    // =====================================================================
    await stopServer();
    startServer();
    ok('the server restarts unlocked', await waitForHealth(), `port ${PORT}`);

    r = await req('/api/users', {
      method: 'POST', body: { name: ADMIN_NAME, password: ADMIN_PASS, role: 'admin' },
    });
    ok('an administrator account can be created', r.status === 200, `${r.status}`);
    adminToken = await login(ADMIN_NAME, ADMIN_PASS);
    await req('/api/users', {
      method: 'POST', token: adminToken,
      body: { name: OP_NAME, password: OP_PASS, role: 'operator' },
    });
    opToken = await login(OP_NAME, OP_PASS);
    ok('...and an operator account', opToken.length > 0,
       `operator token ${opToken ? 'issued' : 'MISSING'}`);

    r = await req('/api/server/config');
    ok('with accounts configured, reading the config needs a token at all',
       r.status === 401, `${r.status}`);

    r = await req('/api/server/config', { token: opToken });
    ok('an operator is refused the machine\'s configuration',
       r.status === 403, `${r.status}`);
    r = await req('/api/server/config', {
      method: 'PATCH', token: opToken, body: { corsOrigin: 'https://operator.example' },
    });
    ok('...and cannot widen the filesystem or CORS policy by writing it',
       r.status === 403 && readConfig().corsOrigin !== 'https://operator.example',
       `${r.status}, stored=${readConfig().corsOrigin}`);

    r = await req('/api/server/config', { token: adminToken });
    ok('an administrator can read it', r.status === 200, `${r.status}`);
    r = await req('/api/server/config', {
      method: 'PATCH', token: adminToken, body: { meterHz: 25 },
    });
    ok('...and write it', r.status === 200 && readConfig().meterHz === 25,
       `${r.status}, meterHz=${readConfig().meterHz}`);

  } catch (e) {
    ok('the suite ran to completion', false, String(e && e.stack || e));
  } finally {
    await stopServer();
    try { fs.rmSync(CONFIG, { force: true }); } catch { /* ignore */ }
    try { fs.rmSync(USERS_JSON, { force: true }); } catch { /* ignore */ }
    try { fs.rmSync(PREFS_DIR, { recursive: true, force: true }); } catch { /* ignore */ }
    try { if (hadCfg)   fs.renameSync(C_BACKUP, CONFIG); } catch { /* ignore */ }
    try { if (hadUsers) fs.renameSync(U_BACKUP, USERS_JSON); } catch { /* ignore */ }
    try { if (hadPrefs) fs.renameSync(P_BACKUP, PREFS_DIR); } catch { /* ignore */ }
  }

  console.log(failures === 0
    ? '\nAll server-configuration assertions passed.'
    : `\n${failures} assertion(s) FAILED.`);
  process.exit(failures === 0 ? 0 : 1);
})();

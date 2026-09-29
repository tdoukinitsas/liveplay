// Profile pictures, and moving the account list between machines.
//
// Two features that sit on the same file (users.json) and the same gate, and
// whose failures are both mostly invisible: a picture route that an operator
// can aim at a colleague looks exactly like one that works, and an export that
// leaks the signing key, or an import that leaves a server with no
// administrator, looks like a successful copy until somebody needs it.
//
// What this pins:
//   • PICTURES: an administrator sets and clears anyone's; a person sets and
//     clears their own through /api/auth/me/avatar; an operator cannot reach a
//     colleague's; an API token cannot reach either route; only small PNG, JPEG
//     and WebP data URLs whose bytes match their label are kept; a picture is
//     not a credential change (sessions survive it); and it survives a restart.
//   • EXPORT: administrators only — never an operator, never a token, never
//     anonymously, and never while the login is off. It carries the Argon2id
//     hashes and the token hashes, and NOT the session-signing secret.
//   • IMPORT: validated completely before anything changes; merge adds what is
//     new and skips name clashes (regenerating a colliding user id); replace
//     swaps the list and says whether the caller is still signed in; neither
//     can leave accounts with no administrator; a fresh machine can take the
//     team without a credential and is locked the moment it has.
//
// Starts its own servers from a COPY of the binary in a temp directory, so the
// users.json it creates and destroys is never the one beside the real build —
// other suites (and other people) may be using that one.
//
//   node server/tests/e2e/users-e2e.js [path-to-liveplay-server]
//   (ports: LIVEPLAY_E2E_PORT, default 4491, and the next one up)
const WebSocket = require(require.resolve('ws', { paths: [process.cwd()] }));
const { spawn } = require('child_process');
const path = require('path');
const fs   = require('fs');
const os   = require('os');

const EXE = process.argv[2] || path.join('server', 'build', 'Release',
  process.platform === 'win32' ? 'liveplay-server.exe' : 'liveplay-server');
const PORT_A = Number(process.env.LIVEPLAY_E2E_PORT) || 4491;
const PORT_B = PORT_A + 1;

const ADMIN = { name: 'e2e-admin',  pass: 'correct-horse-battery' };
const OP1   = { name: 'e2e-op-one', pass: 'stapler-in-the-jelly' };
const OP2   = { name: 'e2e-op-two', pass: 'orange-tractor-lamp' };

let failures = 0;
const ok = (n, p, d) => {
  if (d === undefined) d = '(no detail given — fix the assertion)';
  console.log(`${p ? 'PASS' : 'FAIL'}  ${n}   ${d}`);
  if (!p) failures++;
};
const sleep = ms => new Promise(r => setTimeout(r, ms));

// ---- Pictures --------------------------------------------------------------
// A real 1×1 PNG, so the positive case is an image an <img> would draw rather
// than four magic bytes and noise.
const PNG = 'data:image/png;base64,' +
  'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==';
const b64 = buf => Buffer.from(buf).toString('base64');
const JPEG = 'data:image/jpeg;base64,' + b64([0xFF, 0xD8, 0xFF, 0xE0, 0, 0x10, 0x4A, 0x46, 0x49, 0x46, 0, 1]);
const WEBP = 'data:image/webp;base64,' +
  b64([...Buffer.from('RIFF'), 0x1A, 0, 0, 0, ...Buffer.from('WEBPVP8L'), 0x0D, 0, 0, 0]);
const TOO_BIG = 'data:image/png;base64,' +
  b64(Buffer.concat([Buffer.from([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]),
                     Buffer.alloc(140 * 1024)]));
const SVG      = 'data:image/svg+xml;base64,' + b64(Buffer.from('<svg xmlns="http://www.w3.org/2000/svg"><script>alert(1)</script></svg>'));
const LYING    = 'data:image/png;base64,' + b64([0xFF, 0xD8, 0xFF, 0xE0, 1, 2, 3, 4, 5, 6, 7, 8]);
const NOT_B64  = 'data:image/png;base64,@@@not base64@@@';

// ---- Plumbing ----------------------------------------------------------------
function makeReq(port) {
  return async function req(p, { method = 'GET', token, body } = {}) {
    const headers = {};
    if (token) headers.Authorization = `Bearer ${token}`;
    if (body !== undefined) headers['Content-Type'] = 'application/json';
    try {
      const r = await fetch(`http://127.0.0.1:${port}${p}`, {
        method, headers,
        body: body === undefined ? undefined : JSON.stringify(body),
      });
      const t = await r.text();
      let parsed; try { parsed = JSON.parse(t); } catch { parsed = t; }
      return { status: r.status, body: parsed, text: t, headers: r.headers };
    } catch (e) {
      return { status: 0, body: null, text: '', unreachable: String(e.cause || e) };
    }
  };
}
const reqA = makeReq(PORT_A);
const reqB = makeReq(PORT_B);

async function waitForHealth(port, tries = 40) {
  for (let i = 0; i < tries; i++) {
    try { const r = await fetch(`http://127.0.0.1:${port}/api/health`); if (r.ok) return true; }
    catch { /* not up yet */ }
    await sleep(250);
  }
  return false;
}

// A private copy of the binary and everything it loads, so users.json is
// created beside THAT and nowhere else.
function stage(label) {
  const src = path.dirname(path.resolve(EXE));
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `liveplay-${label}-`));
  for (const f of fs.readdirSync(src)) {
    if (f === path.basename(EXE) || /\.(dll|so|dylib)$/i.test(f)) {
      fs.copyFileSync(path.join(src, f), path.join(dir, f));
    }
  }
  return { dir, exe: path.join(dir, path.basename(EXE)) };
}

function connect(port, token) {
  return new Promise(resolve => {
    const ws = new WebSocket(`ws://127.0.0.1:${port}/ws` +
                             (token ? `?access_token=${encodeURIComponent(token)}` : ''));
    let settled = false;
    const done = v => { if (!settled) { settled = true; resolve(v); } };
    ws.on('open', () => done({ opened: true, ws }));
    ws.on('unexpected-response', (_q, res) => done({ opened: false, status: res.statusCode, ws }));
    ws.on('error', () => done({ opened: false, ws }));
    setTimeout(() => done({ opened: false, status: 'timeout', ws }), 6000);
  });
}
async function waitClosed(c) {
  for (let i = 0; i < 120; i++) {
    if (c.ws.readyState === WebSocket.CLOSED) return true;
    await sleep(25);
  }
  return false;
}
const close = c => { try { c && c.ws && c.ws.close(); } catch { /* gone */ } };

const login = async (req, who) => {
  const r = await req('/api/auth/login', { method: 'POST', body: { name: who.name, password: who.pass } });
  return (r.body && r.body.token) || '';
};
const clone = o => JSON.parse(JSON.stringify(o));

(async () => {
  const A = stage('users-a');
  const B = stage('users-b');
  const procs = {};
  const start = (key, s, port) => {
    procs[key] = spawn(s.exe, ['--port', String(port)], { stdio: 'ignore', windowsHide: true, cwd: s.dir });
  };
  const stop = async key => {
    if (!procs[key]) return;
    try { procs[key].kill(); } catch { /* gone */ }
    procs[key] = null;
    await sleep(1200);
  };

  try {
    start('a', A, PORT_A);
    ok('server A starts from a private copy', await waitForHealth(PORT_A), `${A.dir} :${PORT_A}`);

    // =====================================================================
    // 1. The open posture — nobody to be
    // =====================================================================
    let r = await reqA('/api/auth/me/avatar', { method: 'PUT', body: { avatar: PNG } });
    ok('with no accounts, "my picture" is refused — there is no me',
       r.status === 409, `${r.status} ${r.text}`);
    r = await reqA('/api/users/export');
    ok('...and the export is refused, though every other /api/users route is open',
       r.status === 409, `${r.status} — nothing to export, and nobody signed in to hand it to`);

    r = await reqA('/api/users', { method: 'POST', body: { name: ADMIN.name, password: ADMIN.pass } });
    const adminId = (r.body && r.body.id) || '';
    const adminToken = await login(reqA, ADMIN);
    ok('the first account is created and signs in', !!adminId && adminToken.length > 20, `${r.status}`);

    r = await reqA('/api/users', { method: 'POST', token: adminToken,
                                   body: { name: OP1.name, password: OP1.pass, role: 'operator' } });
    const op1Id = (r.body && r.body.id) || '';
    r = await reqA('/api/users', { method: 'POST', token: adminToken,
                                   body: { name: OP2.name, password: OP2.pass, role: 'operator' } });
    const op2Id = (r.body && r.body.id) || '';
    let op1Token = await login(reqA, OP1);
    ok('two operators exist and one signs in', !!op1Id && !!op2Id && op1Token.length > 20,
       `${op1Id.slice(0, 6)} ${op2Id.slice(0, 6)}`);

    // =====================================================================
    // 2. Pictures
    // =====================================================================
    r = await reqA('/api/users', { token: adminToken });
    ok('GET /api/users carries an avatar field, null until one is set',
       Array.isArray(r.body) && r.body.every(u => 'avatar' in u && u.avatar === null),
       JSON.stringify(Array.isArray(r.body) && r.body.map(u => [u.name, u.avatar])));

    r = await reqA(`/api/users/${op1Id}`, { method: 'PATCH', token: adminToken, body: { avatar: PNG } });
    ok('an administrator sets somebody else\'s picture',
       r.status === 200 && r.body && r.body.avatar === PNG, `${r.status}`);
    r = await reqA('/api/users', { token: adminToken });
    const row = Array.isArray(r.body) ? r.body.find(u => u.id === op1Id) : null;
    ok('...and the list shows it', !!row && row.avatar === PNG, row ? `${String(row.avatar).slice(0, 30)}…` : 'no row');

    r = await reqA('/api/cues', { token: op1Token });
    ok('a new picture is not a new credential — their session still works',
       r.status === 200, `${r.status} — a picture change must not bump the token epoch`);

    r = await reqA('/api/auth/me/avatar', { method: 'PUT', token: op1Token, body: { avatar: JPEG } });
    ok('an OPERATOR sets their OWN picture', r.status === 200 && r.body && r.body.avatar === JPEG,
       `${r.status} ${r.text.slice(0, 80)}`);
    r = await reqA('/api/auth/me', { token: op1Token });
    ok('...and /api/auth/me reports it', r.body && r.body.user && r.body.user.avatar === JPEG,
       JSON.stringify(r.body && r.body.user && String(r.body.user.avatar).slice(0, 30)));

    r = await reqA(`/api/users/${op2Id}`, { method: 'PATCH', token: op1Token, body: { avatar: PNG } });
    ok('an operator can NOT set a colleague\'s picture', r.status === 403, `${r.status}`);

    r = await reqA('/api/auth/me/avatar', { method: 'PUT', token: op1Token, body: { avatar: WEBP } });
    ok('a WebP is accepted too', r.status === 200, `${r.status} ${r.text.slice(0, 80)}`);

    r = await reqA('/api/auth/me/avatar', { method: 'DELETE', token: op1Token });
    ok('an operator clears their own picture', r.status === 200 && r.body && r.body.avatar === null,
       `${r.status}`);
    r = await reqA('/api/auth/me', { token: op1Token });
    ok('...and it is gone', r.body && r.body.user && r.body.user.avatar === null, JSON.stringify(r.body && r.body.user));

    r = await reqA(`/api/users/${op2Id}`, { method: 'PATCH', token: adminToken, body: { avatar: PNG } });
    r = await reqA(`/api/users/${op2Id}`, { method: 'PATCH', token: adminToken, body: { avatar: null } });
    ok('an administrator clears somebody\'s picture with null',
       r.status === 200 && r.body && r.body.avatar === null, `${r.status}`);

    // ---- What is refused ----
    const bad = [];
    for (const [label, avatar, want] of [
      ['too large', TOO_BIG, 413], ['SVG', SVG, 400], ['PNG label on JPEG bytes', LYING, 400],
      ['not base64', NOT_B64, 400], ['a plain URL', 'https://example.com/me.png', 400],
      ['a number', 42, 400],
    ]) {
      r = await reqA('/api/auth/me/avatar', { method: 'PUT', token: op1Token, body: { avatar } });
      if (r.status !== want) bad.push(`self/${label}→${r.status}`);
      r = await reqA(`/api/users/${op1Id}`, { method: 'PATCH', token: adminToken, body: { avatar } });
      if (r.status !== want) bad.push(`admin/${label}→${r.status}`);
    }
    ok('oversized, SVG, mislabelled, undecodable and non-image pictures are all refused',
       bad.length === 0, bad.length ? bad.join(' ') : 'both routes: 413 for size, 400 for the rest');

    r = await reqA(`/api/users/${op1Id}`, { method: 'PATCH', token: adminToken,
                                            body: { name: 'renamed-by-mistake', avatar: SVG } });
    const stillNamed = (await reqA('/api/users', { token: adminToken })).body.some(u => u.name === OP1.name);
    ok('a bad picture in a PATCH changes nothing else in it',
       r.status === 400 && stillNamed, `${r.status} — the name must not change when the request is refused`);

    // ---- API tokens have no face ----
    r = await reqA('/api/tokens', { method: 'POST', token: adminToken,
                                    body: { name: 'e2e-companion', password: ADMIN.pass } });
    const apiToken = (r.body && r.body.token) || '';
    ok('an API token is issued for the next checks', apiToken.startsWith('lpk1_'), `${r.status}`);
    r = await reqA('/api/auth/me/avatar', { method: 'PUT', token: apiToken, body: { avatar: PNG } });
    ok('an API token can NOT set "its own" picture', r.status === 403, `${r.status}`);
    r = await reqA('/api/auth/me/avatar', { method: 'DELETE', token: apiToken });
    ok('...nor clear one', r.status === 403, `${r.status}`);
    r = await reqA(`/api/users/${adminId}`, { method: 'PATCH', token: apiToken, body: { avatar: PNG } });
    ok('...nor anybody\'s through the account routes', r.status === 403, `${r.status}`);

    // Leave a picture on the admin for the export and restart checks.
    await reqA(`/api/users/${adminId}`, { method: 'PATCH', token: adminToken, body: { avatar: PNG } });

    await stop('a');
    start('a', A, PORT_A);
    ok('server A restarts', await waitForHealth(PORT_A), `:${PORT_A}`);
    r = await reqA('/api/auth/me', { token: adminToken });
    ok('a picture survives a restart', r.body && r.body.user && r.body.user.avatar === PNG,
       `${r.status} ${JSON.stringify(r.body && r.body.user && String(r.body.user.avatar).slice(0, 30))}`);

    // =====================================================================
    // 3. Export
    // =====================================================================
    r = await reqA('/api/users/export');
    ok('export with no token is 401', r.status === 401, `${r.status}`);
    r = await reqA('/api/users/export', { token: op1Token });
    ok('export as an OPERATOR is refused', r.status === 403, `${r.status}`);
    r = await reqA('/api/users/export', { token: apiToken });
    ok('export with an API TOKEN is refused', r.status === 403, `${r.status} — tokens never see a hash`);

    r = await reqA('/api/users/export', { token: adminToken });
    const exported = r.body || {};
    ok('an administrator exports', r.status === 200 && exported.format === 'liveplay-users' &&
       exported.version === 1 && typeof exported.exportedAt === 'number',
       `${r.status} format=${exported.format} version=${exported.version}`);
    ok('...every account, with an Argon2id hash, role, id and picture field',
       Array.isArray(exported.users) && exported.users.length === 3 &&
       exported.users.every(u => /^\$argon2id\$/.test(u.hash) && u.id && u.role && 'avatar' in u),
       JSON.stringify((exported.users || []).map(u => [u.name, u.role, String(u.hash).slice(0, 12)])));
    ok('...and the admin\'s picture',
       (exported.users || []).some(u => u.id === adminId && u.avatar === PNG), 'carried inline');
    ok('...and the API token with its hash',
       Array.isArray(exported.apiTokens) && exported.apiTokens.length === 1 &&
       typeof exported.apiTokens[0].hash === 'string' && exported.apiTokens[0].hash.length === 43,
       JSON.stringify((exported.apiTokens || []).map(t => [t.name, String(t.hash).length])));
    ok('...and NOT the session-signing secret, nor the login posture',
       !/tokenSecret/i.test(r.text) && !('authRequired' in exported),
       `keys: ${Object.keys(exported).join(', ')}`);
    ok('...marked not to be cached', /no-store/.test(r.headers.get('cache-control') || ''),
       `Cache-Control: ${r.headers.get('cache-control')}`);

    // =====================================================================
    // 4. Import — validation (nothing may change on a refusal)
    // =====================================================================
    const countA = async () => ((await reqA('/api/users', { token: adminToken })).body || []).length;
    const imp = (data, mode = 'merge', token = adminToken, req = reqA) =>
      req('/api/users/import', { method: 'POST', token, body: { mode, data } });

    r = await imp(exported, 'merge', op1Token);
    ok('import as an OPERATOR is refused', r.status === 403, `${r.status}`);
    r = await imp(exported, 'merge', apiToken);
    ok('import with an API TOKEN is refused', r.status === 403, `${r.status}`);
    r = await reqA('/api/users/import', { method: 'POST', token: adminToken, body: { mode: 'sideways', data: exported } });
    ok('an unknown mode is refused', r.status === 400, `${r.status}`);
    r = await reqA('/api/users/import', { method: 'POST', token: adminToken, body: { mode: 'merge' } });
    ok('no data is refused', r.status === 400, `${r.status}`);

    const broken = [];
    const tamper = (label, fn) => { const d = clone(exported); fn(d); return [label, d]; };
    for (const [label, data] of [
      tamper('wrong format',    d => { d.format = 'liveplay-project'; }),
      tamper('wrong version',   d => { d.version = 2; }),
      tamper('no user list',    d => { delete d.users; }),
      tamper('bcrypt hash',     d => { d.users[0].hash = '$2b$10$abcdefghijklmnopqrstuuABCDEFGHIJKLMNOPQRSTUVWXYZ012'; }),
      tamper('mangled argon2',  d => { d.users[0].hash = '$argon2id$v=19$m=nonsense'; }),
      tamper('unknown role',    d => { d.users[0].role = 'superuser'; }),
      tamper('empty name',      d => { d.users[0].name = '   '; }),
      tamper('duplicate names', d => { d.users[1].name = d.users[0].name.toUpperCase(); }),
      tamper('duplicate ids',   d => { d.users[1].id = d.users[0].id; }),
      tamper('bad id',          d => { d.users[0].id = '../../etc'; }),
      tamper('bad picture',     d => { d.users[0].avatar = SVG; }),
      tamper('bad epoch',       d => { d.users[0].tokenEpoch = -3; }),
      tamper('token bad hash',  d => { d.apiTokens[0].hash = 'short'; }),
      tamper('token id with _', d => { d.apiTokens[0].id = 'abc_def'; }),
    ]) {
      r = await imp(data, 'replace');
      if (r.status !== 400) broken.push(`${label}→${r.status}`);
    }
    ok('every malformed file is refused with 400', broken.length === 0,
       broken.length ? broken.join(' ') : '14 kinds of damage, each named in the reply');
    ok('...and none of them changed anything', await countA() === 3, `${await countA()} accounts`);

    r = await imp({ ...clone(exported), users: [] }, 'replace');
    ok('REPLACE with no accounts is refused — it would switch the login off',
       r.status === 400 && /login off/.test(r.text), `${r.status} ${r.text}`);
    r = await imp({ ...clone(exported), users: clone(exported.users).filter(u => u.role !== 'admin') }, 'replace');
    ok('REPLACE with no administrator is refused', r.status === 409, `${r.status} ${r.text}`);

    // =====================================================================
    // 5. Import — merge
    // =====================================================================
    r = await imp(exported, 'merge');
    ok('merging a file of accounts that are all already here adds nothing',
       r.status === 200 && r.body.usersAdded.length === 0 && r.body.usersSkipped.length === 3 &&
       r.body.tokensAdded.length === 0 && r.body.tokensSkipped.length === 1,
       `${r.status} ${JSON.stringify(r.body)}`);

    // A newcomer whose id happens to be op1's, carrying op2's password hash, and
    // a token that names them as its issuer.
    const op2Row = exported.users.find(u => u.id === op2Id);
    const newcomer = { ...clone(op2Row), id: op1Id, name: 'e2e-newcomer', avatar: WEBP };
    const newToken = { ...clone(exported.apiTokens[0]), id: 'a'.repeat(32), name: 'e2e-imported-token', createdBy: op1Id };
    r = await imp({ ...clone(exported), users: [newcomer], apiTokens: [newToken] }, 'merge');
    ok('merge adds a new account and a new token',
       r.status === 200 && r.body.usersAdded.includes('e2e-newcomer') &&
       r.body.tokensAdded.includes('e2e-imported-token'), `${r.status} ${JSON.stringify(r.body)}`);
    ok('...giving the account a new id because its old one was taken here',
       r.body && r.body.idsRegenerated === 1, `idsRegenerated=${r.body && r.body.idsRegenerated}`);
    const users = (await reqA('/api/users', { token: adminToken })).body || [];
    const nc = users.find(u => u.name === 'e2e-newcomer');
    ok('...and the existing account with that id is untouched',
       !!nc && nc.id !== op1Id && users.find(u => u.id === op1Id).name === OP1.name,
       nc ? `newcomer=${nc.id.slice(0, 8)} op1=${op1Id.slice(0, 8)}` : 'no newcomer');
    const ncToken = await login(reqA, { name: 'e2e-newcomer', pass: OP2.pass });
    ok('...and the imported HASH works: they sign in with the password it was made from',
       ncToken.length > 20, 'no password was chosen on this machine');
    const toks = (await reqA('/api/tokens', { token: adminToken })).body || [];
    const it = toks.find(t => t.name === 'e2e-imported-token');
    ok('...and the imported token\'s issuer followed the new id',
       !!it && it.createdBy === nc.id, JSON.stringify(it));

    // =====================================================================
    // 6. A fresh machine takes the team (server B)
    // =====================================================================
    start('b', B, PORT_B);
    ok('server B starts empty', await waitForHealth(PORT_B), `${B.dir} :${PORT_B}`);
    r = await imp({ ...clone(exported), users: exported.users.filter(u => u.role !== 'admin') },
                  'merge', undefined, reqB);
    ok('a file with no administrator is refused on an empty server too',
       r.status === 409, `${r.status} ${r.text}`);
    r = await imp(exported, 'merge', undefined, reqB);
    ok('an EMPTY server imports without a credential (the bootstrap window)',
       r.status === 200 && r.body.usersAdded.length === 3 && r.body.authRequired === true,
       `${r.status} ${JSON.stringify(r.body)}`);
    r = await reqB('/api/cues');
    ok('...and is locked the moment it has accounts', r.status === 401, `${r.status}`);
    r = await reqB('/api/cues', { token: adminToken });
    ok('...where server A\'s session tokens mean NOTHING (the secret did not travel)',
       r.status === 401, `${r.status}`);
    const bAdmin = await login(reqB, ADMIN);
    ok('...but the same password signs in', bAdmin.length > 20, 'hash carried over');
    r = await reqB('/api/cues', { token: apiToken });
    ok('...and the same Companion token works there without being reissued',
       r.status === 200, `${r.status} — only the hash travelled, and it is all verification needs`);
    r = await reqB('/api/auth/me', { token: bAdmin });
    ok('...with the picture', r.body && r.body.user && r.body.user.avatar === PNG, 'inline in the file');
    await stop('b');

    // =====================================================================
    // 7. Import — replace
    // =====================================================================
    const liveOp1 = await connect(PORT_A, op1Token);
    ok('an operator socket is open before the replace', liveOp1.opened, 'op1');

    const keep = clone(exported.users.find(u => u.id === adminId));
    const fresh = { ...clone(op2Row), id: 'f'.repeat(32), name: 'e2e-fresh' };
    r = await imp({ ...clone(exported), users: [keep, fresh], apiTokens: [] }, 'replace');
    ok('REPLACE swaps the whole list',
       r.status === 200 && r.body.userCount === 2 && r.body.usersRemoved === 3,
       `${r.status} ${JSON.stringify(r.body)}`);
    ok('...and says the caller (same id, same password) is still signed in',
       r.body && r.body.sessionValid === true, `sessionValid=${r.body && r.body.sessionValid}`);
    r = await reqA('/api/cues', { token: adminToken });
    ok('...which is true', r.status === 200, `${r.status}`);
    r = await reqA('/api/cues', { token: op1Token });
    ok('a removed account\'s session stops working', r.status === 401, `${r.status}`);
    ok('...and its open socket is closed', await waitClosed(liveOp1), 'on the next broadcast tick');
    close(liveOp1);
    r = await reqA('/api/cues', { token: apiToken });
    ok('a token not in the file is gone too', r.status === 401, `${r.status}`);

    // The administrator's record comes back with somebody else's password: the
    // same id, a different hash — so their sessions end, as set_password's would.
    const swapped = { ...clone(keep), hash: op2Row.hash };
    r = await imp({ ...clone(exported), users: [swapped, fresh], apiTokens: [] }, 'replace');
    ok('REPLACE with a different password for the caller reports the session as ended',
       r.status === 200 && r.body.sessionValid === false, `${r.status} ${JSON.stringify(r.body)}`);
    r = await reqA('/api/cues', { token: adminToken });
    ok('...which is true', r.status === 401, `${r.status}`);
    ok('...and the imported password is the one that works now',
       (await login(reqA, { name: ADMIN.name, pass: OP2.pass })).length > 20, 'signed in');

  } finally {
    await stop('a');
    await stop('b');
    for (const s of [A, B]) {
      try { fs.rmSync(s.dir, { recursive: true, force: true }); }
      catch (e) { console.log(`note: could not remove ${s.dir}: ${e.message}`); }
    }
  }

  console.log(failures === 0
    ? '\nAll users assertions passed.'
    : `\n${failures} assertion(s) FAILED.`);
  process.exit(failures === 0 ? 0 : 1);
})();

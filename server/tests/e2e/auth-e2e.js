// Who may talk to this server, and what they may change once they have (U3).
//
// The unit adds a users.json beside the executable, Argon2id password hashes,
// signed stateless tokens, and one enforcement point ahead of every REST route.
// Almost all of it is a claim about what is REFUSED, and a refusal that does
// not happen looks exactly like a feature working — so nearly every assertion
// here is negative, and the teeth check at the bottom of the README matters
// more for this suite than for any other.
//
// What this pins:
//   • THE DEFAULT IS OPEN. With no users.json, everything works with no token,
//     exactly as it did before 2.5. An upgrade must not lock a rig out.
//   • The first account can be created without a credential (there is nobody to
//     be an administrator yet) and is forced to admin whatever was asked for.
//   • From that account onward: REST refuses 401 without a token, and — the
//     part a middleware cannot do — the WEBSOCKET refuses the handshake too.
//     Crow runs middleware for an upgrade and then ignores the response, so if
//     .onaccept did not check, play/stop/bus-gain would be reachable with no
//     credential while REST was locked.
//   • The Server tier needs an admin: an operator is refused /api/outputs,
//     /api/users and /api/clients, and allowed everything that runs the show.
//   • Default deny: a path matching NO route still needs a token, so the route
//     table cannot be mapped anonymously.
//   • A tampered token is refused (the signature is checked before the payload
//     is parsed at all).
//   • Revocation is real: changing a password invalidates that user's existing
//     tokens, and deleting a user invalidates theirs.
//   • The last administrator cannot be deleted, which would leave a server that
//     requires a login and nobody able to manage it.
//   • TOKENS SURVIVE A RESTART. This is the whole reason they are signed rather
//     than held in memory: the crash handler auto-restarts this server, and
//     in-memory tokens would sign every surface out mid-show.
//
// Starts its own server (it is testing boot-read policy and a restart), so it
// takes the binary rather than a port, and it OWNS users.json beside that
// binary for the duration — any existing one is moved aside and put back.
//
//   node server/tests/e2e/auth-e2e.js [path-to-liveplay-server]
const WebSocket = require(require.resolve('ws', { paths: [process.cwd()] }));
const { spawn } = require('child_process');
const path = require('path');
const fs   = require('fs');

const EXE = process.argv[2] || path.join('server', 'build', 'Release',
  process.platform === 'win32' ? 'liveplay-server.exe' : 'liveplay-server');

const PORT       = 4573;
const EXE_DIR    = path.dirname(path.resolve(EXE));
const USERS_JSON = path.join(EXE_DIR, 'users.json');
const BACKUP     = USERS_JSON + '.e2e-backup';

const ADMIN_NAME = 'e2e-admin';
const ADMIN_PASS = 'correct-horse-battery';
const OP_NAME    = 'e2e-operator';
const OP_PASS    = 'stapler-in-the-jelly';

let failures = 0;
const ok = (n, p, d) => {
  if (d === undefined) d = '(no detail given — fix the assertion)';
  console.log(`${p ? 'PASS' : 'FAIL'}  ${n}   ${d}`);
  if (!p) failures++;
};
const sleep = ms => new Promise(r => setTimeout(r, ms));

// An unreachable server is a FAILED ASSERTION, not a crashed harness — same
// rule as client-session-e2e, and for the same reason: a run that aborts on the
// first bad fetch reports one line instead of the several that say what broke.
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

// Open a socket, optionally with a token in the query string — which is where
// it has to go, because a browser cannot set headers on a WebSocket handshake.
function connect(token) {
  return new Promise(resolve => {
    const url = `ws://127.0.0.1:${PORT}/ws` +
                (token ? `?access_token=${encodeURIComponent(token)}` : '');
    const ws = new WebSocket(url);
    let settled = false;
    const done = v => { if (!settled) { settled = true; resolve(v); } };
    ws.on('open', () => done({ opened: true, ws }));
    ws.on('unexpected-response', (_req, res) =>
      done({ opened: false, status: res.statusCode, ws }));
    ws.on('error', e =>
      done({ opened: false, status: (e && e.message.match(/\b(\d{3})\b/) || [])[1], ws }));
    setTimeout(() => done({ opened: false, status: 'timeout', ws }), 6000);
  });
}
const close = c => { try { c && c.ws && c.ws.close(); } catch { /* already gone */ } };

(async () => {
  let proc = null;
  const startServer = () => {
    proc = spawn(EXE, ['--port', String(PORT)], { stdio: 'ignore' });
    return proc;
  };
  const stopServer = async () => {
    if (!proc) return;
    try { proc.kill(); } catch { /* already gone */ }
    proc = null;
    // The port has to be free before the next instance tries to bind it.
    await sleep(1200);
  };

  // Move any real users.json aside. This suite creates accounts on the
  // developer's own build directory, and putting back what was there is not
  // optional.
  let hadExisting = false;
  try {
    if (fs.existsSync(USERS_JSON)) {
      fs.renameSync(USERS_JSON, BACKUP);
      hadExisting = true;
    }
  } catch (e) {
    console.log(`FAIL  could not move an existing users.json aside   ${e}`);
    process.exit(1);
  }

  let adminToken = '', opToken = '', opId = '', adminId = '';
  let wsOpen = null, wsAuthed = null;

  try {
    // =====================================================================
    // 1. The default posture — no accounts, nothing changes
    // =====================================================================
    startServer();
    ok('the server starts with no user store', await waitForHealth(), `port ${PORT}`);

    let r = await req('/api/auth/status');
    ok('with no accounts, the server says authentication is NOT required',
       r.status === 200 && r.body && r.body.authRequired === false,
       `${r.status} ${JSON.stringify(r.body)}`);
    ok('...and that it is inviting a first account',
       r.body && r.body.setupRequired === true && r.body.userCount === 0,
       `setupRequired=${r.body && r.body.setupRequired} userCount=${r.body && r.body.userCount}`);

    r = await req('/api/cues');
    ok('an ordinary route works with no token at all (the pre-2.5 behaviour)',
       r.status === 200, `${r.status}`);

    r = await req('/api/clients');
    ok('...and so does an admin-tier route, because nobody is an administrator yet',
       r.status === 200, `${r.status}`);

    wsOpen = await connect();
    ok('...and the socket opens with no credential',
       wsOpen.opened === true, wsOpen.opened ? 'open' : `refused ${wsOpen.status}`);
    close(wsOpen);
    await sleep(300);

    // =====================================================================
    // 2. The first account — creatable without a credential, forced to admin
    // =====================================================================
    // Deliberately asks for 'operator'. A store whose only account cannot
    // manage accounts is a locked room with the key inside, so the store
    // overrides the request.
    r = await req('/api/users', {
      method: 'POST',
      body: { name: ADMIN_NAME, password: ADMIN_PASS, role: 'operator' },
    });
    ok('the first account can be created with no credential',
       r.status === 200, `${r.status} ${JSON.stringify(r.body)}`);
    ok('...and is an ADMINISTRATOR even though "operator" was requested',
       r.body && r.body.role === 'admin', `role=${r.body && r.body.role}`);
    adminId = (r.body && r.body.id) || '';

    r = await req('/api/auth/status');
    ok('the server now says authentication IS required',
       r.status === 200 && r.body && r.body.authRequired === true && r.body.setupRequired === false,
       `authRequired=${r.body && r.body.authRequired} setup=${r.body && r.body.setupRequired}`);

    // =====================================================================
    // 3. Refusals — the half of this unit that is only visible by being denied
    // =====================================================================
    r = await req('/api/cues');
    ok('an ordinary route now REFUSES a request with no token',
       r.status === 401, `${r.status}`);

    r = await req('/api/cues', { token: 'lp1.bogus.bogus' });
    ok('...and refuses a token that is not ours',
       r.status === 401, `${r.status}`);

    r = await req('/api/health');
    ok('health stays public, so a client can still find the server',
       r.status === 200, `${r.status}`);

    r = await req('/api/auth/status');
    ok('...and so does auth/status, or asking whether to log in would need a login',
       r.status === 200, `${r.status}`);

    // Default deny, including for paths that match no route. Anonymous
    // probing must not be able to tell "no such route" from "not yours".
    r = await req('/api/definitely-not-a-route');
    ok('a path matching no route needs a token too (default deny)',
       r.status === 401, `${r.status} — a 404 here would map the route table`);

    // The one a middleware cannot do.
    wsOpen = await connect();
    ok('the WebSocket handshake is REFUSED without a token',
       wsOpen.opened === false, wsOpen.opened ? 'OPENED — the socket is unguarded' : `refused ${wsOpen.status}`);
    close(wsOpen);

    // =====================================================================
    // 4. Logging in
    // =====================================================================
    r = await req('/api/auth/login', {
      method: 'POST', body: { name: ADMIN_NAME, password: 'not the password' },
    });
    ok('a wrong password is refused', r.status === 401, `${r.status}`);
    const wrongPassBody = JSON.stringify(r.body);

    r = await req('/api/auth/login', {
      method: 'POST', body: { name: 'nobody-by-that-name', password: ADMIN_PASS },
    });
    ok('an unknown user is refused', r.status === 401, `${r.status}`);
    ok('...with the SAME message as a wrong password, so the reply cannot be '
       + 'used to enumerate accounts',
       JSON.stringify(r.body) === wrongPassBody,
       `${JSON.stringify(r.body)} vs ${wrongPassBody}`);

    r = await req('/api/auth/login', {
      method: 'POST', body: { name: ADMIN_NAME, password: ADMIN_PASS },
    });
    ok('the right password is accepted', r.status === 200, `${r.status}`);
    adminToken = (r.body && r.body.token) || '';
    ok('...and returns a token', adminToken.length > 20, `${adminToken.length} chars`);
    ok('...and says who it belongs to',
       r.body && r.body.user && r.body.user.name === ADMIN_NAME
                              && r.body.user.role === 'admin',
       JSON.stringify(r.body && r.body.user));

    r = await req('/api/cues', { token: adminToken });
    ok('the token opens the ordinary route', r.status === 200, `${r.status}`);

    wsAuthed = await connect(adminToken);
    ok('...and the socket', wsAuthed.opened === true,
       wsAuthed.opened ? 'open' : `refused ${wsAuthed.status}`);
    await sleep(700);

    r = await req('/api/clients', { token: adminToken });
    const me = Array.isArray(r.body) ? r.body[0] : null;
    ok('and the connected client is reported as that person, not as a number',
       !!me && me.user === ADMIN_NAME && me.isAdmin === true,
       JSON.stringify(me && { user: me.user, isAdmin: me.isAdmin }));
    close(wsAuthed);
    wsAuthed = null;
    await sleep(300);

    // A token whose payload has been edited. The signature covers the encoded
    // body, so any change to it fails to verify — and it fails BEFORE the JSON
    // is parsed, which is what keeps attacker-chosen bytes out of the parser.
    const parts = adminToken.split('.');
    const tampered = [parts[0], parts[1].slice(0, -1) +
                      (parts[1].slice(-1) === 'A' ? 'B' : 'A'), parts[2]].join('.');
    r = await req('/api/cues', { token: tampered });
    ok('a token with an edited payload is refused', r.status === 401, `${r.status}`);

    // =====================================================================
    // 5. Roles — the Server tier needs an administrator
    // =====================================================================
    r = await req('/api/users', {
      method: 'POST', token: adminToken,
      body: { name: OP_NAME, password: OP_PASS, role: 'operator' },
    });
    ok('an admin can create an operator',
       r.status === 200 && r.body && r.body.role === 'operator',
       `${r.status} role=${r.body && r.body.role}`);
    opId = (r.body && r.body.id) || '';

    r = await req('/api/auth/login', {
      method: 'POST', body: { name: OP_NAME, password: OP_PASS },
    });
    opToken = (r.body && r.body.token) || '';
    ok('the operator can log in', r.status === 200 && opToken.length > 20, `${r.status}`);

    r = await req('/api/cues', { token: opToken });
    ok('an operator may run the show', r.status === 200, `${r.status}`);

    const opWs = await connect(opToken);
    ok('...and may hold a socket', opWs.opened === true,
       opWs.opened ? 'open' : `refused ${opWs.status}`);
    close(opWs);
    await sleep(300);

    r = await req('/api/outputs', { token: opToken });
    ok('an operator may NOT rewire the machine (the output map)',
       r.status === 403, `${r.status}`);

    r = await req('/api/users', { token: opToken });
    ok('...nor read the accounts', r.status === 403, `${r.status}`);

    // ---- The shape the Users pane reads (P3c) --------------------------
    // Not a refusal, and not covered anywhere else: the settings pane renders
    // this list directly, so a server that stopped sending `role` or started
    // sending an object instead of an array would leave a blank pane and pass
    // every other assertion in this file.
    r = await req('/api/users', { token: adminToken });
    ok('GET /api/users returns an ARRAY, which is what the pane iterates',
       r.status === 200 && Array.isArray(r.body),
       `${r.status} ${Array.isArray(r.body) ? `${r.body.length} row(s)` : typeof r.body}`);
    const row = Array.isArray(r.body) ? r.body.find(u => u.name === OP_NAME) : null;
    ok('...and every row carries the id, name and role the pane binds to',
       !!row && typeof row.id === 'string' && row.id.length > 0 &&
       typeof row.name === 'string' && (row.role === 'admin' || row.role === 'operator'),
       JSON.stringify(row));
    ok('...and no password hash, which is the one field that must never travel',
       !!row && !('hash' in row) && !('password' in row),
       `fields: [${row ? Object.keys(row).join(', ') : '—'}]`);

    // The pane changes a role by PATCHing {role} alone, and reads the role back
    // off the response to confirm rather than re-fetching.
    r = await req(`/api/users/${opId}`, {
      method: 'PATCH', token: adminToken, body: { role: 'admin' },
    });
    ok('PATCH {role} alone promotes, and answers with the resulting role',
       r.status === 200 && r.body && r.body.role === 'admin',
       `${r.status} role=${r.body && r.body.role}`);
    r = await req(`/api/users/${opId}`, {
      method: 'PATCH', token: adminToken, body: { role: 'operator' },
    });
    ok('...and demotes again, which is what makes the pane\'s select two-way',
       r.status === 200 && r.body && r.body.role === 'operator',
       `${r.status} role=${r.body && r.body.role}`);

    r = await req('/api/clients', { token: opToken });
    ok('...nor see who else is connected, and from where',
       r.status === 403, `${r.status}`);

    r = await req('/api/outputs', { token: adminToken });
    ok('an admin may', r.status === 200, `${r.status}`);

    // =====================================================================
    // 6. Revocation — the reason a stateless token still carries an epoch
    // =====================================================================
    r = await req(`/api/users/${opId}`, {
      method: 'PATCH', token: adminToken, body: { password: 'a-brand-new-password' },
    });
    ok('an admin can change the operator\'s password', r.status === 200, `${r.status}`);

    r = await req('/api/cues', { token: opToken });
    ok('...which INVALIDATES the operator\'s existing token immediately',
       r.status === 401, `${r.status} — a live token after a password change is the bug`);

    r = await req('/api/auth/login', {
      method: 'POST', body: { name: OP_NAME, password: 'a-brand-new-password' },
    });
    opToken = (r.body && r.body.token) || '';
    ok('the operator can log in again with the new password',
       r.status === 200 && opToken.length > 20, `${r.status}`);

    r = await req(`/api/users/${opId}`, { method: 'DELETE', token: adminToken });
    ok('an admin can delete the operator', r.status === 200, `${r.status}`);

    r = await req('/api/cues', { token: opToken });
    ok('...and their token stops working at once',
       r.status === 401, `${r.status}`);

    r = await req(`/api/users/${adminId}`, { method: 'DELETE', token: adminToken });
    ok('the LAST administrator cannot be deleted',
       r.status === 409,
       `${r.status} — allowing it leaves a locked server nobody can manage`);

    // =====================================================================
    // 7. Restart survival — the reason the token is signed, not remembered
    // =====================================================================
    await stopServer();
    startServer();
    ok('the server comes back up', await waitForHealth(), `port ${PORT}`);

    r = await req('/api/auth/status');
    ok('...still requiring authentication (the store was read from disk)',
       r.status === 200 && r.body && r.body.authRequired === true,
       `authRequired=${r.body && r.body.authRequired}`);

    r = await req('/api/cues', { token: adminToken });
    ok('...and the token issued BEFORE the restart still works',
       r.status === 200,
       `${r.status} — the crash handler restarts this server, and an ` +
       `in-memory token would sign the room out mid-show`);

    r = await req('/api/cues');
    ok('...while no token is still refused', r.status === 401, `${r.status}`);

  } finally {
    close(wsAuthed);
    close(wsOpen);
    await stopServer();
    // Put the developer's own store back, and take ours away.
    try {
      if (fs.existsSync(USERS_JSON)) fs.unlinkSync(USERS_JSON);
      if (hadExisting && fs.existsSync(BACKUP)) fs.renameSync(BACKUP, USERS_JSON);
    } catch (e) {
      console.log(`FAIL  could not restore users.json   ${e}`);
      failures++;
    }
  }

  console.log(failures === 0
    ? '\nAll auth assertions passed.'
    : `\n${failures} assertion(s) FAILED.`);
  process.exit(failures === 0 ? 0 : 1);
})();

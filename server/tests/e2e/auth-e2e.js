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
//   • API TOKENS are a principal that is not a person: issued only with the
//     caller's password re-entered, never while authentication is off (which
//     would be persistence rather than convenience), shown exactly once, able
//     to run the show over REST and the socket, and refused the filesystem, the
//     Server tier, and everything that is only true of somebody with hands.
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
async function waitClosed(c) {
  for (let i = 0; i < 80; i++) {
    if (c.ws.readyState === WebSocket.CLOSED) return true;
    await sleep(25);
  }
  return false;
}
const close = c => { try { c && c.ws && c.ws.close(); } catch { /* already gone */ } };

(async () => {
  let proc = null;
  const startServer = () => {
    proc = spawn(EXE, ['--port', String(PORT)], { stdio: 'ignore', windowsHide: true });
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
    await sleep(400);
    r = await req('/api/clients');
    const anon = Array.isArray(r.body) ? r.body[0] : null;
    ok('...and reports as ANONYMOUS rather than as nobody',
       !!anon && anon.kind === 'anonymous' && anon.user === null,
       anon ? `kind=${anon.kind} user=${JSON.stringify(anon.user)}`
            : `no session in ${JSON.stringify(r.body)} — with no accounts there ` +
              `is nobody to be, and a session with no name is still a session`);
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
    // Shape, for the pane that renders this list. It branches on `kind` to tell
    // a person from a machine, and reads remoteIp, locale and connectedSeconds
    // into one line per session — so a dropped field here would blank part of
    // that row while every assertion above still passed.
    ok('...with the fields the connected list binds to',
       !!me && me.kind === 'user' && typeof me.remoteIp === 'string' &&
       typeof me.locale === 'string' && typeof me.connectedSeconds === 'number' &&
       typeof me.userId === 'string',
       me ? `kind=${me.kind} ip=${me.remoteIp} locale=${me.locale} ` +
            `secs=${me.connectedSeconds}` : 'no session');
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

    // =====================================================================
    // 8. Turning authentication OFF, and back on, without losing the team
    // =====================================================================
    // Before this existed there was no route back: auth_required() was "the
    // store is not empty" and the last administrator cannot be deleted (asserted
    // in section 6), so the first account made authentication permanent and the
    // documented recovery was deleting users.json by hand.
    //
    // The whole point is that the accounts SURVIVE, so most of these assertions
    // are about what is still there afterwards rather than about the flag.
    r = await req('/api/users', {
      method: 'POST', token: adminToken,
      body: { name: OP_NAME, password: OP_PASS, role: 'operator' },
    });
    const op2Id = (r.body && r.body.id) || '';
    ok('a fresh operator exists to test the role gate with',
       r.status === 200 && op2Id.length > 0, `${r.status}`);
    r = await req('/api/auth/login', {
      method: 'POST', body: { name: OP_NAME, password: OP_PASS },
    });
    const op2Token = (r.body && r.body.token) || '';

    // ---- What protects the switch ----
    r = await req('/api/auth/required', {
      method: 'PATCH', token: adminToken, body: { required: false },
    });
    ok('turning authentication off WITHOUT a password is refused',
       r.status === 400,
       `${r.status} — the session alone must not be enough: tokens here are ` +
       `long-lived, signed, and cross the LAN with no TLS`);

    r = await req('/api/auth/required', {
      method: 'PATCH', token: adminToken, body: { required: false, password: 'not-it' },
    });
    ok('...and so is a WRONG password, even from a signed-in administrator',
       r.status === 401, `${r.status}`);

    r = await req('/api/auth/required', {
      method: 'PATCH', token: op2Token, body: { required: false, password: OP_PASS },
    });
    ok('...and an OPERATOR is refused (by the middleware, while the door is shut)',
       r.status === 403,
       `${r.status} — 403 not 401: telling somebody their password was wrong ` +
       `when it was right teaches them to distrust the message that matters`);

    // ---- Off ----
    r = await req('/api/auth/required', {
      method: 'PATCH', token: adminToken, body: { required: false, password: ADMIN_PASS },
    });
    ok('an administrator with their password CAN turn authentication off',
       r.status === 200 && r.body && r.body.authRequired === false,
       `${r.status} authRequired=${r.body && r.body.authRequired}`);

    r = await req('/api/auth/status');
    ok('...and the server says so',
       r.status === 200 && r.body && r.body.authRequired === false,
       `authRequired=${r.body && r.body.authRequired}`);
    ok('...WITHOUT throwing the accounts away, which is the whole point',
       r.body && r.body.userCount === 2,
       `userCount=${r.body && r.body.userCount} — 2 expected (admin + operator)`);
    ok('...and it is NOT reported as a fresh installation needing setup',
       r.body && r.body.setupRequired === false,
       `setupRequired=${r.body && r.body.setupRequired} — accounts exist, so ` +
       `the client must not offer to create a first one`);

    r = await req('/api/cues');
    ok('an ordinary route now works with no token, as it did before 2.5',
       r.status === 200, `${r.status}`);
    r = await req('/api/users');
    ok('...and so does an admin-tier route, because the guard is off entirely',
       r.status === 200,
       `${r.status} — stated rather than assumed: this is WHY the switch cannot ` +
       `rely on access_for and checks a password itself`);

    // THE ESCALATION THIS CLOSES, and it is only reachable in this state.
    // Everything admin-tier is open right now, so without a rule of its own
    // anyone on the network could mint a credential that does not expire and
    // keep it working after an administrator shut the door again. The refusal
    // lives in the STORE rather than in the route, so no future caller misses it.
    r = await req('/api/tokens', {
      method: 'POST', body: { name: 'e2e-through-the-open-door', password: ADMIN_PASS },
    });
    ok('an API token CANNOT be issued while authentication is off',
       r.status === 409,
       `${r.status} — a 200 here means a token minted through the open door ` +
       `survives the door being shut, which is persistence, not convenience`);

    r = await req('/api/tokens');
    ok('...while LISTING them still works, so nothing hides from an administrator',
       r.status === 200 && Array.isArray(r.body) && r.body.length === 0,
       `${r.status} ${JSON.stringify(r.body)}`);

    // THE ROLE CHECK THAT ACTUALLY MATTERS, and it is only reachable here.
    // With the door shut, an operator aiming at this route is stopped by the
    // middleware and the handler never runs — so the assertion above proves
    // access_for, not the handler. With the door OPEN there is no middleware
    // gate and no token at all: an operator's password is the only thing between
    // them and locking the building out of its own desk. (This assertion is here
    // because disabling the handler's is_admin() check turned NOTHING red.)
    r = await req('/api/auth/required', {
      method: 'PATCH', body: { required: true, name: OP_NAME, password: OP_PASS },
    });
    ok('...but an OPERATOR still cannot turn the login ON with the door open',
       r.status === 403,
       `${r.status} — 403 expected. A 200 here means any account on the rig can ` +
       `change the server's posture once somebody has opened it`);

    r = await req('/api/auth/status');
    ok('...and the refusal changed nothing',
       r.status === 200 && r.body && r.body.authRequired === false,
       `authRequired=${r.body && r.body.authRequired}`);

    // ---- It survives a restart, which is the difference between a posture
    //      and a runtime flag ----
    await stopServer();
    startServer();
    ok('the server comes back up with authentication still off',
       await waitForHealth(), `port ${PORT}`);
    r = await req('/api/auth/status');
    ok('...read back from users.json, not defaulted',
       r.status === 200 && r.body && r.body.authRequired === false &&
       r.body.userCount === 2,
       `authRequired=${r.body && r.body.authRequired} ` +
       `userCount=${r.body && r.body.userCount}`);

    const anonymousLive = await connect();
    ok('an anonymous socket is open before login is enabled', anonymousLive.opened, 'open posture');
    // ---- Back on ----
    r = await req('/api/auth/required', { method: 'PATCH', body: { required: true } });
    ok('turning it back ON without a password is refused too',
       r.status === 400,
       `${r.status} — otherwise anyone on the LAN could lock the desk mid-show ` +
       `while the door was open`);

    r = await req('/api/auth/required', {
      method: 'PATCH', body: { required: true, name: ADMIN_NAME, password: 'not-it' },
    });
    ok('...and a wrong password is refused with no session to fall back on',
       r.status === 401, `${r.status}`);

    r = await req('/api/auth/required', {
      method: 'PATCH', body: { required: true, name: ADMIN_NAME, password: ADMIN_PASS },
    });
    ok('an administrator\'s NAME and password turn it back on with no token at all',
       r.status === 200 && r.body && r.body.authRequired === true,
       `${r.status} authRequired=${r.body && r.body.authRequired}`);

    r = await req('/api/cues');
    ok('...and the door is shut again', r.status === 401, `${r.status}`);
    ok('enabling login closes an ALREADY OPEN anonymous socket',
       await waitClosed(anonymousLive), 'idle connections must lose access too');
    close(anonymousLive);

    r = await req('/api/cues', { token: adminToken });
    ok('...while the token issued BEFORE all of this still works',
       r.status === 200,
       `${r.status} — a posture change is not a revocation, and signing every ` +
       `surface out to flip a switch would be its own outage`);

    r = await req('/api/users', { token: adminToken });
    ok('...and both accounts came through unchanged',
       r.status === 200 && Array.isArray(r.body) && r.body.length === 2 &&
       r.body.some(u => u.name === ADMIN_NAME && u.role === 'admin') &&
       r.body.some(u => u.name === OP_NAME && u.role === 'operator'),
       `${r.status} ${Array.isArray(r.body)
          ? r.body.map(u => `${u.name}/${u.role}`).join(' ') : typeof r.body}`);

    // =====================================================================
    // 9. API tokens — a credential for a thing rather than a person
    // =====================================================================
    // §6.2 of the ownership model calls these "principals that are not people".
    // The claims worth pinning are all about the EDGES of the thing, because
    // the middle (it authenticates, the show runs) fails loudly on its own:
    //   • an operator cannot issue one, and neither can a token;
    //   • the caller's password is re-entered, because a sniffed admin session
    //     must not convert into a credential that never expires;
    //   • the secret exists in exactly one response and is never stored;
    //   • it runs the show but never reaches the disk or the Server tier;
    //   • revoking is immediate, and surviving a restart is the whole point.
    r = await req('/api/tokens', { token: op2Token });
    ok('an operator cannot list API tokens',
       r.status === 403, `${r.status} — they decide who may talk to this machine`);

    r = await req('/api/tokens', {
      method: 'POST', token: op2Token, body: { name: 'e2e-op', password: OP_PASS },
    });
    ok('...nor issue one', r.status === 403, `${r.status}`);

    r = await req('/api/tokens', {
      method: 'POST', token: adminToken, body: { name: 'e2e-companion' },
    });
    ok('issuing WITHOUT the caller\'s password is refused',
       r.status === 400,
       `${r.status} — the session alone must not mint a credential that outlives it`);

    r = await req('/api/tokens', {
      method: 'POST', token: adminToken, body: { name: 'e2e-companion', password: 'not-it' },
    });
    ok('...and so is a WRONG password from a signed-in administrator',
       r.status === 401, `${r.status}`);

    r = await req('/api/tokens', {
      method: 'POST', token: adminToken,
      body: { name: 'e2e-companion', password: ADMIN_PASS },
    });
    const apiToken   = (r.body && r.body.token) || '';
    const apiTokenId = (r.body && r.body.id)    || '';
    ok('an administrator with their password issues a token',
       r.status === 200 && apiToken.startsWith('lpk1_') && apiTokenId.length > 0,
       `${r.status} ${apiToken ? apiToken.slice(0, 12) + '…' : JSON.stringify(r.body)}`);

    r = await req('/api/tokens', {
      method: 'POST', token: adminToken,
      body: { name: 'E2E-Companion', password: ADMIN_PASS },
    });
    ok('...and a second token cannot take the same name',
       r.status === 409,
       `${r.status} — two called "Companion" is the state in which somebody ` +
       `revokes the wrong one mid-show`);

    r = await req('/api/tokens', { token: adminToken });
    const listed = Array.isArray(r.body) ? r.body.find(t => t.id === apiTokenId) : null;
    ok('the listing shows it', r.status === 200 && !!listed,
       `${r.status} ${JSON.stringify(r.body)}`);
    ok('...and the secret appears in NO listing, ever',
       !!listed && !('token' in listed) && !('hash' in listed) && !('secret' in listed) &&
       !JSON.stringify(r.body).includes(apiToken.slice(5)),
       `keys: ${listed ? Object.keys(listed).join(',') : 'none'} — the store kept ` +
       `a BLAKE2b hash; there is no secret here to leak`);
    ok('...and it has never been used yet',
       !!listed && listed.lastUsedAt === null,
       `lastUsedAt=${listed && listed.lastUsedAt} — null, not 0, so a listing ` +
       `cannot render the epoch and call it 1970`);

    // ---- What it CAN do: run the show ----
    r = await req('/api/cues', { token: apiToken });
    ok('the token authenticates an ordinary route', r.status === 200, `${r.status}`);

    r = await req('/api/auth/me', { token: apiToken });
    ok('...and /api/auth/me says WHICH kind of credential asked',
       r.status === 200 && r.body && r.body.user && r.body.user.kind === 'token' &&
       r.body.user.name === 'e2e-companion',
       `${r.status} ${JSON.stringify(r.body && r.body.user)}`);

    r = await req('/api/transport/stop_all', { method: 'POST', token: apiToken });
    ok('...and transport control, which is what automation is FOR',
       r.status === 200, `${r.status}`);

    // The user's amendment to the deny list: opening and saving the show is the
    // automation everybody actually wants. 400 not 403 — it reached the handler
    // and was refused for having no path, which is the proof that it got there.
    r = await req('/api/project/load', { method: 'POST', token: apiToken, body: {} });
    ok('...and LOADING a project is allowed to reach the handler',
       r.status === 400,
       `${r.status} — 403 would mean the deny list swallowed the one filesystem-ish ` +
       `thing a Companion button legitimately does`);
    r = await req('/api/project/save', { method: 'POST', token: apiToken, body: {} });
    ok('...as is saving one', r.status === 400, `${r.status}`);

    // ---- What it CANNOT do: the disk, and anything only a person has ----
    const refused = [];
    for (const p of ['/api/fs/list', '/api/upload', '/api/file/download',
                     '/api/copy_to_media', '/api/project/import', '/api/project/export']) {
      r = await req(p, { token: apiToken });
      if (r.status !== 403) refused.push(`${p}→${r.status}`);
    }
    ok('a token never reaches the FILESYSTEM',
       refused.length === 0,
       refused.length ? refused.join(' ')
                      : 'list, upload, download, copy-to-media, import and export ' +
                        'all 403 — the promise §6.2 makes in writing');

    const notPeople = [];
    for (const p of ['/api/prefs', '/api/auth/logout_all', '/api/auth/required',
                     '/api/tokens']) {
      r = await req(p, { token: apiToken });
      if (r.status !== 403) notPeople.push(`${p}→${r.status}`);
    }
    ok('...nor anything that is only true of a PERSON',
       notPeople.length === 0,
       notPeople.length ? notPeople.join(' ')
                        : 'preferences, sign-out-everywhere, the login posture, and ' +
                          'issuing another token are all 403');

    r = await req('/api/users', { token: apiToken });
    ok('...nor the Server tier, because a token is never an administrator',
       r.status === 403,
       `${r.status} — it is issued at the operator tier and has no role to raise`);

    // ---- The socket, which is the low-latency half of the same job ----
    const wsToken = await connect(apiToken);
    ok('a token may open the WebSocket',
       wsToken.opened === true,
       wsToken.opened ? 'open — play/stop/bus gain is the operator tier in full'
                      : `refused ${wsToken.status}`);
    await sleep(400);
    r = await req('/api/clients', { token: adminToken });
    const asToken = Array.isArray(r.body) ? r.body.find(c => c.kind === 'token') : null;
    ok('...and it is reported as a TOKEN in the connected list, by name',
       !!asToken && asToken.user === 'e2e-companion' && asToken.isAdmin === false,
       asToken ? JSON.stringify(asToken)
               : `no token session among ${JSON.stringify(r.body)} — an operator ` +
                 `must not take a machine for a colleague they could ask to close a window`);
    close(wsToken);
    await sleep(300);

    // ---- Bad credentials ----
    // Tamper with the FIRST character of the secret, not the last, and the
    // reason is worth keeping: the secret is 32 bytes in 43 unpadded base64
    // characters, so 258 bits of spelling carry 256 bits of value and the final
    // character's low two bits are not read at all. libsodium decodes such a
    // string happily, which means flipping the last character can yield the
    // SAME 32 bytes — this assertion passed by luck on its first run and failed
    // on the second, with nothing changed. The first character's six bits are
    // all significant, so this one is a different token every time.
    const apiSecret = apiToken.slice(apiToken.indexOf('_', 5) + 1);
    const apiTampered = apiToken.slice(0, apiToken.length - apiSecret.length) +
                        (apiSecret[0] === 'A' ? 'B' : 'A') + apiSecret.slice(1);
    r = await req('/api/cues', { token: apiTampered });
    ok('one wrong character is not the token',
       r.status === 401, `${r.status}`);
    r = await req('/api/cues', { token: `lpk1_${'0'.repeat(32)}_${'A'.repeat(43)}` });
    ok('...and a token id that was never issued is refused',
       r.status === 401, `${r.status}`);

    // ---- Renaming, and the restart that is the whole point ----
    r = await req(`/api/tokens/${apiTokenId}`, {
      method: 'PATCH', token: adminToken, body: { name: 'e2e-companion-foh' },
    });
    ok('a token can be renamed without reissuing it',
       r.status === 200, `${r.status} — the label is for the human, not the machine`);

    await stopServer();
    startServer();
    ok('the server comes back up', await waitForHealth(), `port ${PORT}`);

    r = await req('/api/cues', { token: apiToken });
    ok('THE TOKEN SURVIVES A RESTART',
       r.status === 200,
       `${r.status} — a Companion install runs for seasons, and the crash handler ` +
       `restarts this server; a credential that died here would be useless`);

    r = await req('/api/tokens', { token: adminToken });
    const after = Array.isArray(r.body) ? r.body.find(t => t.id === apiTokenId) : null;
    ok('...under its new name, with its use recorded on disk',
       !!after && after.name === 'e2e-companion-foh' && typeof after.lastUsedAt === 'number',
       after ? JSON.stringify(after) : JSON.stringify(r.body));

    const liveToken = await connect(apiToken);
    const liveAdmin = await connect(adminToken);
    const busBefore = (await req('/api/buses', { token: adminToken })).body.find(b => b.master);
    ok('both sockets are open before revocation', liveToken.opened && liveAdmin.opened, 'token and admin');
    // ---- Revocation, which is the only way one ends ----
    r = await req(`/api/tokens/${apiTokenId}`, { method: 'DELETE', token: adminToken });
    ok('an administrator revokes it', r.status === 200, `${r.status}`);
    if (liveToken.ws.readyState === WebSocket.OPEN) {
      liveToken.ws.send(JSON.stringify({ type: 'bus_gain', busId: busBefore.id, gainDb: -37 }));
    }
    ok('revocation closes an ALREADY OPEN token socket',
       await waitClosed(liveToken), 'no reconnect needed for revocation');
    const busAfter = (await req(`/api/buses/${busBefore.id}`, { token: adminToken })).body;
    ok('a revoked socket cannot change bus gain', busAfter.gainDb === busBefore.gainDb,
       `${busBefore.gainDb} -> ${busAfter.gainDb}`);
    ok('revoking a token leaves another authenticated socket open',
       liveAdmin.ws.readyState === WebSocket.OPEN, 'admin connection survives');
    close(liveToken);
    close(liveAdmin);

    r = await req('/api/cues', { token: apiToken });
    ok('...and it stops working IMMEDIATELY',
       r.status === 401,
       `${r.status} — verification is a lookup, so there is no window in which a ` +
       `revoked token still answers and nothing to wait out`);

    const wsGone = await connect(apiToken);
    ok('...on the socket too', wsGone.opened === false,
       wsGone.opened ? 'the handshake still succeeded' : `refused ${wsGone.status}`);
    close(wsGone);
    await sleep(300);

    r = await req(`/api/tokens/${apiTokenId}`, { method: 'DELETE', token: adminToken });
    ok('...and revoking it twice is a 404, not a second success',
       r.status === 404, `${r.status}`);

    r = await req('/api/cues', { token: adminToken });
    ok('...while the administrator\'s own session is untouched',
       r.status === 200,
       `${r.status} — revoking a machine's credential must not sign out the person`);

    // ---- The shared login throttle ----
    // LAST, because it deliberately leaves this address blocked for 30 seconds.
    //
    // The claim being tested: this route cannot be used as an unthrottled
    // password oracle sitting beside a throttled front door.
    //
    // Run with authentication ON and a valid admin token, which is what makes
    // the guesses reach the handler at all. Two things learned by getting this
    // wrong first:
    //   * While auth is ON, an UNAUTHENTICATED call here is refused by the
    //     middleware before the handler runs, so those attempts never reach the
    //     throttle — they are middleware 401s, not wrong passwords.
    //   * While auth is OFF, /api/auth/login answers 409 ("no accounts in use")
    //     before it consults the throttle, so the shared counter cannot be
    //     OBSERVED from the front door in that state.
    // Both directions are reachable at once only here, with the door shut and a
    // token in hand.
    let throttled = 0;
    for (let i = 0; i < 6; i++) {
      r = await req('/api/auth/required', {
        method: 'PATCH', token: adminToken, body: { required: false, password: `guess-${i}` },
      });
      if (r.status === 429) { throttled = i + 1; break; }
    }
    ok('repeated wrong confirmations on this route are RATE LIMITED',
       throttled > 0 && throttled <= 6,
       throttled > 0 ? `429 on attempt ${throttled}`
                     : 'never throttled — this route would be an unthrottled ' +
                       'password oracle sitting beside a throttled front door');

    r = await req('/api/auth/login', {
      method: 'POST', body: { name: ADMIN_NAME, password: ADMIN_PASS },
    });
    ok('...by the SAME counter the login uses — a CORRECT password is refused too',
       r.status === 429,
       `${r.status} — one brake per address, not one per route`);

    r = await req('/api/auth/required', {
      method: 'PATCH', token: adminToken, body: { required: false, password: ADMIN_PASS },
    });
    ok('...and the right password does not get past the block here either',
       r.status === 429,
       `${r.status} — the throttle is checked before the password, so guessing ` +
       `cannot be laundered through a correct attempt`);

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

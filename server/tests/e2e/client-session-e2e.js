// A connection is somebody, not a pointer in a set (U1) — and the WebSocket
// obeys the origin policy the REST surface already obeyed.
//
// `ws_clients` was an unordered_set<connection*>: the server knew how MANY
// clients it had and nothing else about any of them. Everything the ownership
// model calls the User tier — a locale that is this operator's rather than the
// server's, a meter rate this surface asked for, a principal that says what a
// client may do — needs somewhere to hang, and there was nowhere. It is now a
// map to a ClientSession, and this suite is the read surface's proof.
//
// The second half is a hole S2 left open. CORS is a rule browsers apply to XHR
// and fetch; the WebSocket handshake is exempt from it. So an installation
// started with --cors-origin https://console.example had its REST surface
// restricted and its socket wide open — and the socket carries play, stop, bus
// gain, mute and selection. `.onaccept` is where that closes, and closing it is
// what stops the seam being dead code until U3 needs it.
//
// What this pins:
//   • two connections are two sessions, with distinct ids and their own rows;
//   • a disconnect removes exactly its own row;
//   • ids are monotonic and never reused, so an id in a log means one session;
//   • the initial playback_snapshot STILL arrives — U1 folded the parallel
//     "pending snapshot" set into the session, and that is the one refactor
//     here that could silently break a client;
//   • with an origin configured: a foreign Origin is refused, the configured
//     one is admitted, and no Origin at all is admitted (native clients and
//     Companion send none, and a browser cannot suppress its own);
//   • with the default `*`: any Origin is admitted, so nothing changes on
//     upgrade for an installation that never set the flag.
//
// Starts its own servers (it is testing boot-configured policy), so it takes
// the binary rather than a port.
//
//   node server/tests/e2e/client-session-e2e.js [path-to-liveplay-server]
const WebSocket = require(require.resolve('ws', { paths: [process.cwd()] }));
const { spawn } = require('child_process');
const path = require('path');

const EXE = process.argv[2] || path.join('server', 'build', 'Release',
  process.platform === 'win32' ? 'liveplay-server.exe' : 'liveplay-server');

const OPEN_PORT   = 4571;   // default posture: --cors-origin unset, so `*`
const LOCKED_PORT = 4572;   // --cors-origin https://console.example
const ALLOWED     = 'https://console.example';

let failures = 0;
const ok = (n, p, d) => {
  if (d === undefined) d = '(no detail given — fix the assertion)';
  console.log(`${p ? 'PASS' : 'FAIL'}  ${n}   ${d}`);
  if (!p) failures++;
};
const sleep = ms => new Promise(r => setTimeout(r, ms));

// An unreachable server is a FAILED ASSERTION, not a crashed harness — the
// same rule boot-config-e2e follows, and for the same reason: a run that
// aborts on the first bad fetch reports one line instead of the several that
// would say which claim broke.
const rest = async (port, p) => {
  try {
    const r = await fetch(`http://127.0.0.1:${port}${p}`);
    const t = await r.text();
    let body; try { body = JSON.parse(t); } catch { body = t; }
    return { status: r.status, body };
  } catch (e) {
    return { status: 0, body: null, unreachable: String(e.cause || e) };
  }
};
const clients = async port => {
  const r = await rest(port, '/api/clients');
  return Array.isArray(r.body) ? r.body : [];
};

async function waitForHealth(port, tries = 40) {
  for (let i = 0; i < tries; i++) {
    try { const r = await fetch(`http://127.0.0.1:${port}/api/health`); if (r.ok) return true; }
    catch { /* not up yet */ }
    await sleep(250);
  }
  return false;
}

// Open a socket and resolve with what happened. `origin` undefined means send
// no Origin header at all, which is what every native client does — distinct
// from sending an empty one.
function connect(port, origin) {
  return new Promise(resolve => {
    const opts = origin === undefined ? {} : { origin };
    const ws = new WebSocket(`ws://127.0.0.1:${port}/ws`, opts);
    const frames = [];
    let settled = false;
    const done = v => { if (!settled) { settled = true; resolve(v); } };
    ws.on('message', raw => {
      try { frames.push(JSON.parse(raw)); } catch { /* non-JSON is not ours */ }
    });
    ws.on('open',  () => done({ opened: true, ws, frames }));
    // ws surfaces the refusal status on 'unexpected-response'; 'error' carries
    // it too on some paths, so both settle and the status is read from either.
    ws.on('unexpected-response', (_req, res) =>
      done({ opened: false, status: res.statusCode, ws, frames }));
    ws.on('error', e =>
      done({ opened: false, status: (e && e.message.match(/\b(\d{3})\b/) || [])[1], ws, frames }));
    setTimeout(() => done({ opened: false, status: 'timeout', ws, frames }), 6000);
  });
}
const close = c => { try { c && c.ws && c.ws.close(); } catch { /* already gone */ } };

// A session row is only meaningful once the server has processed the open, and
// once the broadcast tick has had a chance to deliver the snapshot.
const settle = () => sleep(700);

(async () => {
  const procs = [];
  const start = args => {
    const p = spawn(EXE, args, { stdio: 'ignore' });
    procs.push(p);
    return p;
  };

  let a, b, c;
  try {
    start(['--port', String(OPEN_PORT)]);
    start(['--port', String(LOCKED_PORT), '--cors-origin', ALLOWED]);
    ok('the default-posture server starts', await waitForHealth(OPEN_PORT), `port ${OPEN_PORT}`);
    ok('the origin-locked server starts',   await waitForHealth(LOCKED_PORT), `port ${LOCKED_PORT}`);

    // ---- 1. Nobody is connected yet --------------------------------------
    let list = await clients(OPEN_PORT);
    ok('with no sockets open, the server reports no clients',
       list.length === 0, `${list.length} client(s)`);

    // ---- 2. Two connections are two sessions -----------------------------
    // Connected ONE AT A TIME so each id can be learned by diffing the list,
    // rather than assuming the first socket opened is the first registered.
    // It is not: the client's 'open' fires when it receives the 101, and the
    // server's onopen runs on its own io_context thread, so two back-to-back
    // connects can be registered in either order. Assuming otherwise made this
    // suite fail against a correct server, which is the wrong kind of red.
    a = await connect(OPEN_PORT);
    ok('the first socket opens', a.opened === true, a.opened ? 'open' : `refused ${a.status}`);
    await settle();
    list = await clients(OPEN_PORT);
    ok('and one client is reported', list.length === 1,
       `${list.length}: ${JSON.stringify(list.map(x => x.id))}`);
    const idA = list.length ? list[0].id : -1;

    b = await connect(OPEN_PORT);
    ok('the second socket opens', b.opened === true, b.opened ? 'open' : `refused ${b.status}`);
    await settle();

    list = await clients(OPEN_PORT);
    ok('and the server reports two clients', list.length === 2,
       `${list.length}: ${JSON.stringify(list.map(x => x.id))}`);
    const ids = list.map(x => x.id);
    ok('each with its own id', new Set(ids).size === 2, JSON.stringify(ids));
    const idB = ids.find(x => x !== idA);
    ok('...and each with the address it came from',
       list.every(x => typeof x.remoteIp === 'string' && x.remoteIp.length > 0),
       JSON.stringify(list.map(x => x.remoteIp)));
    ok('...and a connected time that is a number, not a placeholder',
       list.every(x => typeof x.connectedSeconds === 'number' && x.connectedSeconds >= 0),
       JSON.stringify(list.map(x => x.connectedSeconds)));

    // ---- 3. The snapshot still arrives -----------------------------------
    // The one refactor here that could break a client silently: the "needs a
    // snapshot" set was folded into the session record. If the flag is never
    // read, a connecting client gets meters but never learns what is already
    // playing, and nothing else in this suite would notice.
    ok('a connecting client still receives its playback_snapshot',
       a.frames.some(f => f && f.type === 'playback_snapshot'),
       `frame types seen: ${JSON.stringify([...new Set(a.frames.map(f => f && f.type))])}`);

    // ---- 4. A disconnect removes exactly its own row ---------------------
    close(a);
    await settle();
    list = await clients(OPEN_PORT);
    ok('closing one socket leaves one client', list.length === 1,
       `${list.length}: ${JSON.stringify(list.map(x => x.id))}`);
    ok('...and it is the OTHER one that remains, not just any one',
       list.length === 1 && list[0].id === idB,
       `closed #${idA}, expected #${idB}, remaining ${JSON.stringify(list.map(x => x.id))}`);

    // ---- 5. Ids are not reused; the clock runs --------------------------
    // A recycled id would make a log line ambiguous about which session it
    // described, which is most of what an id is for.
    c = await connect(OPEN_PORT);
    await settle();
    list = await clients(OPEN_PORT);
    const highest = Math.max(...list.map(x => x.id));
    ok('a new connection takes a fresh id, never the closed one',
       !list.some(x => x.id === idA) && highest > Math.max(...ids),
       `closed #${idA}, now ${JSON.stringify(list.map(x => x.id))}`);
    // connectedSeconds being a number is cheap; being an ELAPSED number is the
    // claim. The surviving socket has been open across several settles by now.
    const survivor = list.find(x => x.id === idB);
    ok('and the surviving session has been connected measurably longer',
       !!survivor && survivor.connectedSeconds >= 1 &&
       survivor.connectedSeconds > (list.find(x => x.id === highest)?.connectedSeconds ?? 99),
       `#${idB} ${survivor?.connectedSeconds}s vs new #${highest} ` +
       `${list.find(x => x.id === highest)?.connectedSeconds}s`);
    close(b); close(c);

    // ---- 6. The default posture admits any origin ------------------------
    // An installation that never set the flag must see no change on upgrade.
    const openForeign = await connect(OPEN_PORT, 'https://somewhere.else');
    ok('with the default `*`, a foreign origin is still admitted',
       openForeign.opened === true,
       openForeign.opened ? 'opened' : `refused ${openForeign.status}`);
    close(openForeign);

    // ---- 7. A configured origin is enforced on the socket ----------------
    const foreign = await connect(LOCKED_PORT, 'https://evil.example');
    ok('with an origin configured, a foreign browser origin is REFUSED',
       foreign.opened === false,
       foreign.opened ? 'opened — the socket ignored the policy' : `refused ${foreign.status}`);
    ok('...with 403, so a console can tell policy from a bad request',
       String(foreign.status) === '403', `status ${foreign.status}`);
    close(foreign);

    const allowed = await connect(LOCKED_PORT, ALLOWED);
    ok('the configured origin is admitted',
       allowed.opened === true,
       allowed.opened ? 'opened' : `refused ${allowed.status}`);
    close(allowed);

    // Native clients, Companion, curl and the Electron app send no Origin, and
    // refusing them would break every control surface that is not a browser
    // tab. Safe because a browser cannot suppress its own Origin.
    const none = await connect(LOCKED_PORT);
    ok('a client sending no Origin at all is admitted',
       none.opened === true,
       none.opened ? 'opened' : `refused ${none.status}`);
    close(none);

    await settle();
    const lockedList = await clients(LOCKED_PORT);
    ok('and the refused upgrades left no sessions behind',
       lockedList.length === 0, `${lockedList.length} client(s)`);
  } finally {
    close(a); close(b); close(c);
    for (const p of procs) { try { p.kill(); } catch { /* already gone */ } }
    await sleep(300);
  }

  console.log(`\n${failures === 0 ? 'ALL PASS' : 'FAILURES'} (${failures})`);
  process.exit(failures === 0 ? 0 : 1);
})().catch(e => { console.error('harness error:', e); process.exit(2); });

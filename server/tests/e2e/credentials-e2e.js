// Credentials in event connections (audit F-01, F-23, F-30, Q8), against
// servers this suite owns (it creates an account, and runs a second server
// with a different data folder to stand in for another machine):
//   * no client is sent a credential: an operator sees dots for secret header
//     values (and the rest as typed), an API token sees no header value at
//     all, on every path: the document, the item list, connection lists, and
//     the WebSocket patches;
//   * saving what you were shown keeps the stored value, and so does pasting
//     an item whose event carries dots;
//   * the show file holds them sealed, never in plain text, and the same
//     server opens them again;
//   * another machine can't: the connection asks for a sign-in until the
//     credentials are entered again;
//   * a device that answers 401 flags the connection "rejected", and new
//     credentials clear it;
//   * an API token can't change connections;
//   * another show pushed without connections doesn't inherit these.
//
// usage: node credentials-e2e.js <port> [serverExe]
//        (uses <port> and <port>+1; never 4480: that may be the live app)
const fs   = require('fs');
const os   = require('os');
const path = require('path');
const http = require('http');
const { spawn } = require('child_process');

const PORT = Number(process.argv[2]);
if (!PORT || PORT === 4480 || PORT + 1 === 4480) {
  console.error('usage: node credentials-e2e.js <port: it and the next must not be 4480> [serverExe]');
  process.exit(2);
}
const EXE = path.resolve(process.argv[3] || path.join(__dirname, '../../build/Release/liveplay-server.exe'));
const MASK = '••••••';

let failures = 0;
const ok = (name, pass, detail = '') => {
  console.log(`${pass ? 'PASS' : 'FAIL'}  ${name}   ${detail}`);
  if (!pass) failures++;
};
const sleep = ms => new Promise(r => setTimeout(r, ms));

const client = (port) => {
  let token = null;
  const req = async (p, { method = 'GET', body, as } = {}) => {
    const headers = { 'content-type': 'application/json' };
    const t = as === undefined ? token : as;
    if (t) headers.Authorization = `Bearer ${t}`;
    const r = await fetch(`http://127.0.0.1:${port}${p}`, {
      method, headers, body: body === undefined ? undefined : JSON.stringify(body),
    });
    const text = await r.text();
    let parsed; try { parsed = JSON.parse(text); } catch { parsed = text; }
    while (parsed && parsed.pending && parsed.jobId) {
      await sleep(50);
      const next = await fetch(`http://127.0.0.1:${port}/api/events/jobs/${parsed.jobId}`, { headers });
      parsed = await next.json();
    }
    return { status: r.status, body: parsed, text };
  };
  return { req, setToken: (t) => { token = t; } };
};

function writeWav(file, seconds) {
  const rate = 48000, ch = 2, n = Math.floor(rate * seconds);
  const data = Buffer.alloc(n * ch * 2);
  const h = Buffer.alloc(44);
  h.write('RIFF', 0); h.writeUInt32LE(36 + data.length, 4); h.write('WAVE', 8);
  h.write('fmt ', 12); h.writeUInt32LE(16, 16); h.writeUInt16LE(1, 20); h.writeUInt16LE(ch, 22);
  h.writeUInt32LE(rate, 24); h.writeUInt32LE(rate * ch * 2, 28); h.writeUInt16LE(ch * 2, 32);
  h.writeUInt16LE(16, 34); h.write('data', 36); h.writeUInt32LE(data.length, 40);
  fs.writeFileSync(file, Buffer.concat([h, data]));
}

const startServer = async (port, dataDir) => {
  const proc = spawn(EXE, ['--port', String(port), '--bind', '127.0.0.1', '--data-dir', dataDir],
                     { stdio: 'ignore', detached: true, windowsHide: true });
  proc.unref();
  for (let i = 0; i < 100; i++) {
    try { if ((await fetch(`http://127.0.0.1:${port}/api/health`)).ok) break; } catch { /* not yet */ }
    await sleep(100);
  }
  return proc;
};

const watchPatches = (port) => {
  const frames = [];
  const ws = new WebSocket(`ws://127.0.0.1:${port}/ws`);
  ws.addEventListener('message', (e) => {
    if (typeof e.data === 'string') frames.push(e.data);
  });
  return { frames, close: () => ws.close(), ready: new Promise(r => ws.addEventListener('open', r)) };
};

(async () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'liveplay-creds-'));
  const dataA = path.join(root, 'data-a');
  const dataB = path.join(root, 'data-b');
  fs.mkdirSync(dataA); fs.mkdirSync(dataB);
  const showDir = path.join(root, 'show');
  fs.mkdirSync(showDir);
  const folder = showDir.replace(/\\/g, '/');
  const wav = path.join(showDir, 'tone.wav');
  writeWav(wav, 0.4);
  const showFile = path.join(showDir, 'show.liveplay').replace(/\\/g, '/');

  // The device: remembers what it was sent, refuses any token but "good".
  const seen = [];
  const device = http.createServer((rq, rs) => {
    seen.push({ auth: rq.headers.authorization, key: rq.headers['x-api-key'], accept: rq.headers.accept });
    if (rq.headers.authorization !== 'Bearer good') { rs.statusCode = 401; rs.end('no'); return; }
    rs.end('ok');
  });
  await new Promise(r => device.listen(0, '127.0.0.1', r));
  const devicePort = device.address().port;

  const connection = (headers) => ({
    id: 'h1', name: 'Device', plugin: 'http', enabled: true,
    config: { baseUrl: `http://127.0.0.1:${devicePort}`, headers, timeoutMs: 1000 },
  });
  const cue = {
    uuid: 'cue-1', type: 'audio', displayName: 'Cue', mediaServerPath: wav.replace(/\\/g, '/'),
    endBehavior: { action: 'nothing' }, duckingBehavior: { mode: 'no-ducking' },
    events: [{ id: 'ev-1', trigger: 'start', plugin: 'http', action: 'request', connectionId: 'h1',
               options: { method: 'GET', path: '/go', headers: 'X-Api-Key: k123', contentType: 'none' } }],
  };

  let A = null, B = null;
  const procs = [];
  try {
    procs.push(A = await startServer(PORT, dataA));
    const a = client(PORT);
    const fireCue = async (c) => {
      seen.length = 0;
      await c.req('/api/project/items/cue-1/play', { method: 'POST', body: {} });
      await sleep(400);
      await c.req('/api/transport/stop_all', { method: 'POST', body: { fade_ms: 0 } });
      await sleep(150);
      return seen[seen.length - 1];
    };

    let r = await a.req('/api/project/document', { method: 'PUT', body: {
      name: 'creds', version: '2.6.0', busSchema: 2, folderPath: folder,
      eventConnections: [connection('Authorization: Bearer good\nAccept: text/plain')],
      items: [cue],
    } });
    ok('show accepted', r.status === 200, `HTTP ${r.status}`);
    await sleep(700);

    // ---- What an operator sees --------------------------------------------
    r = await a.req('/api/events/connections');
    let headers = r.body.connections?.[0]?.config?.headers;
    ok('an operator sees the credential as dots, the rest as typed',
       headers === `Authorization: ${MASK}\nAccept: text/plain`, JSON.stringify(headers));
    r = await a.req('/api/project');
    ok("the document carries no credential (connection or event)",
       !r.text.includes('Bearer good') && !r.text.includes('k123') && r.text.includes(MASK),
       r.text.length + ' bytes');
    r = await a.req('/api/project/items');
    ok("the item list carries no event credential (F-01)",
       !r.text.includes('k123') && r.text.includes(`X-Api-Key: ${MASK}`));

    let last = await fireCue(a);
    ok('the device gets the real credentials', last && last.auth === 'Bearer good' && last.key === 'k123',
       JSON.stringify(last));

    // ---- Saving what you were shown ---------------------------------------
    r = await a.req('/api/events/connections');
    await a.req('/api/events/connections', { method: 'PUT', body: { connections: r.body.connections } });
    r = await a.req('/api/project');
    const shownCue = (r.body.project || r.body).items.find(i => i.uuid === 'cue-1');
    const patches = watchPatches(PORT);
    await patches.ready;
    await a.req('/api/project/items/cue-1', { method: 'PATCH', body: { events: shownCue.events } });
    await sleep(200);
    last = await fireCue(a);
    ok('saving what you were shown keeps every credential', last && last.auth === 'Bearer good' &&
       last.key === 'k123', JSON.stringify(last));
    const itemPatch = patches.frames.find(f => f.includes('"item_updated"') || f.includes('cue-1'));
    ok('an item patch on the socket carries dots, not the credential (F-01)',
       !!itemPatch && !itemPatch.includes('k123'), itemPatch ? itemPatch.slice(0, 120) : 'no patch seen');
    patches.close();

    // A pasted copy (new uuid and event id) carries dots: the stored value is found.
    r = await a.req('/api/project/items', { method: 'POST', body: {
      item: { ...shownCue, uuid: 'cue-2', displayName: 'Copy', cueId: undefined,
              events: shownCue.events.map(e => ({ ...e, id: 'ev-2' })) } } });
    const copyR = await a.req('/api/project');
    const copy = (copyR.body.project || copyR.body).items.find(i => i.uuid === 'cue-2');
    ok('a pasted copy is accepted', !!copy, `HTTP ${r.status}`);

    // ---- On disk ------------------------------------------------------------
    r = await a.req('/api/project/save', { method: 'POST', body: { path: showFile } });
    ok('show saved', r.status === 200, `HTTP ${r.status}`);
    const onDisk = fs.readFileSync(showFile, 'utf8');
    ok('the file holds no credential in plain text',
       !onDisk.includes('Bearer good') && !onDisk.includes('k123'), '');
    ok('...but sealed ones', (onDisk.match(/lpenc:v1:/g) || []).length >= 2);
    ok('...and the key stays in the data folder', fs.existsSync(path.join(dataA, 'secret.key')));
    r = await a.req('/api/project/load', { method: 'POST', body: { path: showFile } });
    await sleep(700);
    last = await fireCue(a);
    ok('the same server opens them again', last && last.auth === 'Bearer good' && last.key === 'k123',
       JSON.stringify(last));

    // ---- Another machine ------------------------------------------------------
    procs.push(B = await startServer(PORT + 1, dataB));
    const b = client(PORT + 1);
    const bPatches = watchPatches(PORT + 1);
    await bPatches.ready;
    r = await b.req('/api/project/load', { method: 'POST', body: { path: showFile } });
    await sleep(700);
    r = await b.req('/api/events/connections');
    ok("another machine can't open them: the connection asks for a sign-in",
       r.body.connections?.[0]?.auth === 'missing', JSON.stringify(r.body.connections?.[0]?.auth));
    last = await fireCue(b);
    ok('...and nothing sealed is ever sent', seen.length === 0, JSON.stringify(seen));
    const fired = bPatches.frames.map(f => { try { return JSON.parse(f); } catch { return {}; } })
      .find(f => f.op === 'event_fired' && f.ok === false);
    ok('...the failed event says why', fired?.auth === 'missing', JSON.stringify(fired));

    // Sign in again: the operator replaces the dots with the credential.
    let conns = (await b.req('/api/events/connections')).body.connections;
    conns[0].config.headers = 'Authorization: Bearer good\nAccept: text/plain';
    r = await b.req('/api/events/connections', { method: 'PUT', body: { connections: conns } });
    ok('entering the connection credential is accepted', r.status === 200);
    r = await b.req('/api/events/connections');
    ok("...but not while an event's own credential is still sealed",
       r.body.connections?.[0]?.auth === 'missing', JSON.stringify(r.body.connections?.[0]?.auth));
    r = await b.req('/api/project');
    for (const uuid of ['cue-1', 'cue-2']) {
      const item = (r.body.project || r.body).items.find(i => i.uuid === uuid);
      item.events[0].options.headers = 'X-Api-Key: k123';
      await b.req(`/api/project/items/${uuid}`, { method: 'PATCH', body: { events: item.events } });
    }
    await sleep(200);
    r = await b.req('/api/events/connections');
    ok('...entering those too clears the sign-in', !r.body.connections?.[0]?.auth,
       JSON.stringify(r.body.connections?.[0]));
    last = await fireCue(b);
    ok('...and the device gets them', last && last.auth === 'Bearer good' && last.key === 'k123',
       JSON.stringify(last));

    // ---- Refused --------------------------------------------------------------
    conns = (await b.req('/api/events/connections')).body.connections;
    conns[0].config.headers = 'Authorization: Bearer wrong';
    await b.req('/api/events/connections', { method: 'PUT', body: { connections: conns } });
    bPatches.frames.length = 0;
    await fireCue(b);
    await sleep(200);
    r = await b.req('/api/events/connections');
    ok('a 401 flags the connection "rejected"', r.body.connections?.[0]?.auth === 'rejected',
       JSON.stringify(r.body.connections?.[0]?.auth));
    ok('...and every client hears it',
       bPatches.frames.some(f => f.includes('event_connections_changed') && f.includes('"rejected"')));
    const t = await b.req('/api/events/test', { method: 'POST', body: {
      event: { plugin: 'http', action: 'request', connectionId: 'h1',
               options: { method: 'GET', path: '/t', headers: '', contentType: 'none' } } } });
    ok('a Test says the credentials were refused', t.body.ok === false && t.body.auth === 'rejected',
       JSON.stringify(t.body));
    conns = (await b.req('/api/events/connections')).body.connections;
    conns[0].config.headers = 'Authorization: Bearer good';
    await b.req('/api/events/connections', { method: 'PUT', body: { connections: conns } });
    r = await b.req('/api/events/connections');
    ok('new credentials clear it', !r.body.connections?.[0]?.auth, JSON.stringify(r.body.connections?.[0]));
    bPatches.close();

    // ---- Another show doesn't inherit them (Q8) ---------------------------
    r = await b.req('/api/project/document', { method: 'PUT', body: {
      name: 'other show', version: '2.0.0', busSchema: 2, folderPath: folder + '/elsewhere', items: [],
    } });
    r = await b.req('/api/events/connections');
    ok("another show pushed without connections doesn't inherit these",
       (r.body.connections || []).length === 0, JSON.stringify(r.body.connections));

    // ---- API tokens -------------------------------------------------------------
    await a.req('/api/users', { method: 'POST', body: { name: 'credadmin', password: 'cred-test-pass-1', role: 'admin' } });
    r = await a.req('/api/auth/login', { method: 'POST', body: { name: 'credadmin', password: 'cred-test-pass-1' } });
    const admin = r.body?.token;
    a.setToken(admin);
    r = await a.req('/api/tokens', { method: 'POST', body: { name: 'cred-test', password: 'cred-test-pass-1' } });
    const apiToken = r.body?.token;
    ok('an API token could be created', !!apiToken, `HTTP ${r.status}`);
    r = await a.req('/api/project', { as: apiToken });
    ok('an API token sees no header value at all',
       r.status === 200 && !r.text.includes('Bearer good') && !r.text.includes('k123') &&
       !r.text.includes('text/plain') && r.text.includes(`Accept: ${MASK}`), `HTTP ${r.status}`);
    r = await a.req('/api/events/connections', { as: apiToken });
    r = await a.req('/api/events/connections', { method: 'PUT', as: apiToken,
                                                 body: { connections: r.body.connections } });
    ok("an API token can't change connections (F-30)", r.status === 403, `HTTP ${r.status}`);
  } catch (e) {
    ok('the suite ran to the end', false, e?.stack ?? String(e));
  } finally {
    for (const p of procs) { try { process.kill(p.pid); } catch { /* gone */ } }
    device.close();
    await sleep(500);
    try { fs.rmSync(root, { recursive: true, force: true }); } catch { /* still closing */ }
  }

  console.log(failures ? `\n${failures} check(s) failed` : '\nAll checks passed');
  process.exit(failures ? 1 : 0);
})();

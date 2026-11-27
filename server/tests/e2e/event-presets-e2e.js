// Event presets (#84): one named set of settings on a connection, used by any
// number of events. What has to hold, against a server this suite owns (it
// creates an account, which must not happen on a server anyone else uses):
//   * an event on a preset fires the preset's action and options;
//   * changing the preset changes what that event does, with no edit to the cue;
//   * an event's own option overrides that one field only;
//   * a preset that has gone refuses to fire rather than firing something else;
//   * malformed presets are dropped, and a show without presets keeps its shape;
//   * no client sees a password in a preset's headers (an API token sees no
//     header value at all), saving what you were shown does not wipe the real
//     one, and a token can't change connections (F-30). credentials-e2e.js
//     covers the rest of the credential rules.
//
// usage: node event-presets-e2e.js <port> [serverExe]
//        (never 4480: that may be the live app)
const fs    = require('fs');
const os    = require('os');
const path  = require('path');
const dgram = require('dgram');
const { spawn } = require('child_process');

const PORT = process.argv[2];
if (!PORT || PORT === '4480') {
  console.error('usage: node event-presets-e2e.js <port other than 4480> [serverExe]');
  process.exit(2);
}
const EXE = path.resolve(process.argv[3] || path.join(__dirname, '../../build/Release/liveplay-server.exe'));
const BASE = `http://127.0.0.1:${PORT}`;

let failures = 0;
const ok = (name, pass, detail = '') => {
  console.log(`${pass ? 'PASS' : 'FAIL'}  ${name}   ${detail}`);
  if (!pass) failures++;
};
const sleep = ms => new Promise(r => setTimeout(r, ms));

async function req(p, { method = 'GET', token, body } = {}) {
  const headers = { 'content-type': 'application/json' };
  if (token) headers.Authorization = `Bearer ${token}`;
  const r = await fetch(BASE + p, {
    method, headers, body: body === undefined ? undefined : JSON.stringify(body),
  });
  const text = await r.text();
  let parsed; try { parsed = JSON.parse(text); } catch { parsed = text; }
  while (parsed && parsed.pending && parsed.jobId) {
    await sleep(50);
    const next = await fetch(`${BASE}/api/events/jobs/${parsed.jobId}`, { headers });
    parsed = await next.json();
  }
  return { status: r.status, body: parsed };
}

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

(async () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'liveplay-presets-'));
  const dataDir = path.join(root, 'data');
  fs.mkdirSync(dataDir);
  const wav = path.join(root, 'tone.wav');
  writeWav(wav, 1);

  const osc = [];
  const udp = dgram.createSocket('udp4');
  udp.on('message', m => osc.push(m.toString('utf8', 0, m.indexOf(0))));
  await new Promise(r => udp.bind(0, '127.0.0.1', r));
  const oscConfig = { host: '127.0.0.1', port: udp.address().port };

  const server = spawn(EXE, ['--port', PORT, '--bind', '127.0.0.1', '--data-dir', dataDir],
                       { stdio: 'ignore', detached: true, windowsHide: true });
  server.unref();

  const message = address => ({ address, args: [] });
  const connection = (presets) => ({
    id: 'osc-1', name: 'OSC', plugin: 'osc', enabled: true, config: oscConfig, presets,
  });
  const fired = async (event) => {
    osc.length = 0;
    const r = await req('/api/events/test', { method: 'POST', body: {
      event: { plugin: 'osc', connectionId: 'osc-1', ...event },
    } });
    await sleep(120);
    return { r: r.body, got: [...osc] };
  };

  try {
    for (let i = 0; i < 80; i++) {
      try { if ((await fetch(`${BASE}/api/health`)).ok) break; } catch { /* not yet */ }
      await sleep(100);
    }

    await req('/api/project/document', { method: 'PUT', body: {
      name: 'presets', eventConnections: [connection([
        { id: 'p1', name: 'One', action: 'message', options: message('/preset/one') },
      ])],
      items: [{
        uuid: 'cue-1', type: 'audio', displayName: 'Cue', mediaServerPath: wav.replace(/\\/g, '/'),
        endBehavior: { action: 'nothing' },
        events: [{ id: 'e1', trigger: 'start', plugin: 'osc', action: '', connectionId: 'osc-1',
                   presetId: 'p1', options: {} }],
      }],
    } });
    await sleep(600);

    osc.length = 0;
    await req('/api/project/items/cue-1/play', { method: 'POST', body: {} });
    await sleep(250);
    ok('a cue fires its preset', osc.includes('/preset/one'), osc.join(','));
    await req('/api/transport/stop_all', { method: 'POST', body: {} });
    await sleep(200);

    await req('/api/events/connections', { method: 'PUT', body: { connections: [connection([
      { id: 'p1', name: 'One', action: 'message', options: message('/preset/changed') },
    ])] } });
    osc.length = 0;
    await req('/api/project/items/cue-1/play', { method: 'POST', body: {} });
    await sleep(250);
    ok('changing the preset changes the cue, with no edit to it',
       osc.includes('/preset/changed') && !osc.includes('/preset/one'), osc.join(','));
    await req('/api/transport/stop_all', { method: 'POST', body: {} });
    await sleep(200);

    let t = await fired({ action: '', presetId: 'p1', options: {} });
    ok('Test fires the preset', t.r.ok === true && t.got.includes('/preset/changed'), t.got.join(','));
    t = await fired({ action: '', presetId: 'p1', options: { address: '/override' } });
    ok("an event's own option overrides that field", t.got.includes('/override'), t.got.join(','));
    t = await fired({ action: 'message', presetId: 'gone', options: message('/must/not/fire') });
    ok('a missing preset refuses to fire', t.r.ok === false && /preset/.test(t.r.error || '') &&
       t.got.length === 0, JSON.stringify(t.r));
    t = await fired({ action: 'message', options: message('/plain') });
    ok('an event with no preset is unaffected', t.got.includes('/plain'), t.got.join(','));

    let r = await req('/api/events/connections', { method: 'PUT', body: { connections: [connection([
      { id: 'good', name: 'Good', action: 'message', options: message('/x') },
      { id: 'good', name: 'Duplicate id', action: 'message', options: message('/y') },
      { id: '', name: 'No id', action: 'message', options: {} },
      { id: 'no-action', name: 'No action', options: {} },
      'not a preset',
    ])] } });
    const kept = r.body.connections[0].presets ?? [];
    ok('malformed presets are dropped', kept.length === 1 && kept[0].name === 'Good',
       JSON.stringify(kept.map(p => p.name)));
    r = await req('/api/events/connections', { method: 'PUT', body: { connections: [connection([])] } });
    ok('a connection with no presets keeps its old shape', !('presets' in r.body.connections[0]));

    // ---- Secrets -----------------------------------------------------------
    const secret = 'Authorization: Bearer super-secret-value';
    const http = {
      id: 'http-1', name: 'Hook', plugin: 'http', enabled: true,
      config: { baseUrl: 'http://127.0.0.1:9', headers: '', timeoutMs: 200 },
      presets: [{ id: 'hp', name: 'Hook', action: 'request',
                  options: { method: 'POST', path: '/', headers: secret } }],
    };
    await req('/api/events/connections', { method: 'PUT', body: { connections: [http] } });
    await req('/api/users', { method: 'POST', body: { name: 'presetadmin', password: 'preset-test-pass-1', role: 'admin' } });
    r = await req('/api/auth/login', { method: 'POST', body: { name: 'presetadmin', password: 'preset-test-pass-1' } });
    const admin = r.body?.token;
    r = await req('/api/tokens', { method: 'POST', token: admin,
                                   body: { name: 'preset-test', password: 'preset-test-pass-1' } });
    const apiToken = r.body?.token;
    ok('an API token could be created', !!apiToken, `HTTP ${r.status}`);

    r = await req('/api/events/connections', { token: apiToken });
    const seen = JSON.stringify(r.body);
    ok("an API token can't read a preset's password", r.status === 200 &&
       !seen.includes('super-secret-value') && seen.includes('Authorization'), seen.slice(0, 200));
    const shown = r.body.connections;
    r = await req('/api/events/connections', { method: 'PUT', token: apiToken, body: { connections: shown } });
    ok("an API token can't change connections (F-30)", r.status === 403, `HTTP ${r.status}`);
    r = await req('/api/events/connections', { token: admin });
    const adminSees = r.body.connections?.[0]?.presets?.[0]?.options?.headers;
    ok("an operator can't read it either: dots (F-23)",
       typeof adminSees === 'string' && adminSees.startsWith('Authorization: ') &&
       !adminSees.includes('super-secret-value'), String(adminSees));
    r = await req('/api/events/connections', { method: 'PUT', token: admin,
                                               body: { connections: r.body.connections } });
    ok('an operator saving what they were shown is accepted', r.status === 200, `HTTP ${r.status}`);
  } catch (e) {
    ok('the suite ran to the end', false, e?.stack ?? String(e));
  } finally {
    try { process.kill(server.pid); } catch { /* gone */ }
    udp.close();
    await sleep(400);
    try { fs.rmSync(root, { recursive: true, force: true }); } catch { /* still closing */ }
  }

  console.log(failures ? `\n${failures} check(s) failed` : '\nAll checks passed');
  process.exit(failures ? 1 : 0);
})();

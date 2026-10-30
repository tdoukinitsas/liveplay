// Event system (#84), end-to-end against a running server.
//
// Listens for OSC on a local UDP port and for HTTP on a local TCP port, points
// a project's connections at them, and plays a 3 s tone carrying events:
//   start, time 1.0, time 2.0, pause, resume, end   → OSC
//   time 0.5                                         → HTTP POST with {item.name}
// Checks that each arrives, and that markers land on their mark (the
// sequencer polls every 50 ms; markers are scheduled ahead, so they should be
// much closer than that). Then pauses across a marker (it must wait for the
// resume), tests a draft connection, checks that a document saved without
// eventConnections keeps them, and that legacy customActions are migrated.
//
// No audio device needed for the timing: the playhead runs regardless.
// usage: node events-e2e.js <port>
const fs    = require('fs');
const os    = require('os');
const path  = require('path');
const dgram = require('dgram');
const http  = require('http');

const PORT = process.argv[2] || '4480';
const BASE = `http://127.0.0.1:${PORT}`;
const MARK_TOLERANCE_MS = 40;
let failures = 0;
const ok = (name, pass, detail = '') => {
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

function writeWav(file, seconds) {
  const rate = 48000, ch = 2, n = rate * seconds;
  const data = Buffer.alloc(n * ch * 2);
  for (let i = 0; i < n; i++) {
    const v = Math.round(Math.sin(2 * Math.PI * 440 * i / rate) * 0.2 * 32767);
    data.writeInt16LE(v, i * 4); data.writeInt16LE(v, i * 4 + 2);
  }
  const h = Buffer.alloc(44);
  h.write('RIFF', 0); h.writeUInt32LE(36 + data.length, 4); h.write('WAVE', 8);
  h.write('fmt ', 12); h.writeUInt32LE(16, 16); h.writeUInt16LE(1, 20); h.writeUInt16LE(ch, 22);
  h.writeUInt32LE(rate, 24); h.writeUInt32LE(rate * ch * 2, 28); h.writeUInt16LE(ch * 2, 32);
  h.writeUInt16LE(16, 34); h.write('data', 36); h.writeUInt32LE(data.length, 40);
  fs.writeFileSync(file, Buffer.concat([h, data]));
}

// Minimal OSC 1.0 decoder: address, type tags, i/f/s args.
function decodeOsc(buf) {
  let off = 0;
  const str = () => {
    const end = buf.indexOf(0, off);
    const s = buf.toString('utf8', off, end);
    off = (end + 4) & ~3;
    return s;
  };
  const address = str();
  const tags = str();
  const args = [];
  for (const t of tags.slice(1)) {
    if (t === 'i') { args.push(buf.readInt32BE(off)); off += 4; }
    else if (t === 'f') { args.push(buf.readFloatBE(off)); off += 4; }
    else if (t === 's') args.push(str());
    else if (t === 'T') args.push(true);
    else if (t === 'F') args.push(false);
  }
  return { address, args };
}

const oscEvent = (id, trigger, address, extra = {}) => ({
  id, trigger, plugin: 'osc', action: 'message', connectionId: 'osc-1',
  options: { address, args: [] }, ...extra,
});

(async () => {
  // ---- listeners ----
  const osc = [];
  const udp = dgram.createSocket('udp4');
  udp.on('message', m => osc.push({ t: Date.now(), ...decodeOsc(m) }));
  await new Promise(r => udp.bind(0, '127.0.0.1', r));
  const oscPort = udp.address().port;

  const hits = [];
  const srv = http.createServer((req, res) => {
    let body = '';
    req.on('data', c => { body += c; });
    req.on('end', () => {
      hits.push({ t: Date.now(), method: req.method, url: req.url, body,
                  type: req.headers['content-type'], key: req.headers['x-key'] });
      res.end('ok');
    });
  });
  await new Promise(r => srv.listen(0, '127.0.0.1', r));
  const httpPort = srv.address().port;

  // ---- project ----
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'lp-events-'));
  const wav = path.join(dir, 'tone.wav'); writeWav(wav, 3);
  const connections = [
    { id: 'osc-1', name: 'OSC rx', plugin: 'osc', enabled: true,
      config: { host: '127.0.0.1', port: oscPort } },
    { id: 'http-1', name: 'HTTP rx', plugin: 'http', enabled: true,
      config: { baseUrl: `http://127.0.0.1:${httpPort}`, headers: 'X-Key: abc', timeoutMs: 1000 } },
  ];
  const doc = {
    name: 'events-e2e',
    eventConnections: connections,
    items: [
      { uuid: 'ev-a', type: 'audio', displayName: 'Tone A', mediaServerPath: wav,
        manualStopFade: 0, fadeOutDuration: 0, endBehavior: { action: 'nothing' },
        events: [
          oscEvent('e-start', 'start', '/lp/start'),
          oscEvent('e-t1', 'time', '/lp/t1', { time: 1.0,
            options: { address: '/lp/t1', args: [{ type: 'i', value: '42' },
                                                 { type: 's', value: '{item.name}' },
                                                 { type: 'f', value: 0.5 }] } }),
          oscEvent('e-t2', 'time', '/lp/t2', { time: 2.0 }),
          oscEvent('e-pause', 'pause', '/lp/pause'),
          oscEvent('e-resume', 'resume', '/lp/resume'),
          oscEvent('e-end', 'end', '/lp/end'),
          oscEvent('e-off', 'time', '/lp/disabled', { time: 0.2, enabled: false }),
          { id: 'e-http', trigger: 'time', time: 0.5, plugin: 'http', action: 'request',
            connectionId: 'http-1',
            options: { method: 'POST', path: '/hook?x=1', headers: '',
                       contentType: 'application/json', body: '{"cue":"{item.name}"}' } },
        ] },
      // Legacy customActions must come back as events.
      { uuid: 'ev-legacy', type: 'audio', displayName: 'Legacy', mediaServerPath: wav,
        customActions: [
          { timePoint: 1.5, action: { type: 'stop-all' } },
          { timePoint: 2.5, action: { type: 'http-request', request: {
              method: 'POST', url: 'http://10.0.0.9:8080/api/go', contentType: 'json',
              body: { a: 1 } } } },
        ] },
    ],
  };
  let r = await rest('/api/project/document', { method: 'PUT', body: JSON.stringify(doc) });
  ok('document accepted', r.status === 200, `status ${r.status}`);
  await sleep(1500);   // async decode

  // ---- schemas ----
  r = await rest('/api/events/plugins');
  const ids = (r.body.plugins || []).map(p => p.id);
  ok('plugins listed', ['osc', 'midi', 'http', 'xair', 'liveplay'].every(id => ids.includes(id)),
     ids.join(','));

  // Live option lists and discovery, without a device: an unreachable mixer
  // still yields the static list, with the reason.
  r = await rest('/api/events/options', { method: 'POST', body: JSON.stringify({
    plugin: 'xair', source: 'strips', connection: { config: { host: '127.0.0.1', port: 9 } },
  }) });
  ok('options fall back with an error', (r.body.options || []).length === 32 && !!r.body.error,
     `${(r.body.options || []).length} options, error: ${r.body.error}`);
  r = await rest('/api/events/options', { method: 'POST', body: JSON.stringify({ plugin: 'nope', source: 'x' }) });
  ok('options for an unknown plugin', !!r.body.error && (r.body.options || []).length === 0, r.body.error);
  r = await rest('/api/events/discover', { method: 'POST', body: JSON.stringify({ plugin: 'osc', timeoutMs: 200 }) });
  ok('discover on a plugin without discovery', Array.isArray(r.body.devices) && r.body.devices.length === 0);

  // ---- a full pass ----
  osc.length = 0; hits.length = 0;
  const t0 = Date.now();
  await rest('/api/project/items/ev-a/play', { method: 'POST', body: '{}' });
  await sleep(3600);
  const at = addr => osc.find(m => m.address === addr);
  const start = at('/lp/start');
  ok('start fired', !!start, start ? `+${start.t - t0} ms` : 'missing');
  // Markers are measured from the start event, which fires as playback begins.
  const base = start ? start.t : t0;
  for (const [addr, sec] of [['/lp/t1', 1], ['/lp/t2', 2]]) {
    const m = at(addr);
    const err = m ? (m.t - base) - sec * 1000 : NaN;
    ok(`${addr} on its mark`, m && Math.abs(err) <= MARK_TOLERANCE_MS,
       m ? `${err >= 0 ? '+' : ''}${err} ms` : 'missing');
  }
  const t1 = at('/lp/t1');
  ok('osc args encoded', t1 && t1.args[0] === 42 && t1.args[1] === 'Tone A' &&
     Math.abs(t1.args[2] - 0.5) < 1e-6, t1 ? JSON.stringify(t1.args) : 'missing');
  ok('disabled event skipped', !at('/lp/disabled'));
  const end = at('/lp/end');
  ok('end fired on natural end', !!end, end ? `+${end.t - base} ms` : 'missing');
  ok('end fired once', osc.filter(m => m.address === '/lp/end').length === 1);
  const hook = hits[0];
  ok('http request sent', hook && hook.method === 'POST' && hook.url === '/hook?x=1' &&
     hook.body === '{"cue":"Tone A"}' && hook.key === 'abc' &&
     /application\/json/.test(hook.type || ''),
     hook ? JSON.stringify(hook) : 'missing');

  // ---- pause across a marker ----
  osc.length = 0;
  await rest('/api/project/items/ev-a/play', { method: 'POST', body: '{}' });
  await sleep(1500);
  await rest('/api/project/items/ev-a/pause', { method: 'POST', body: '{}' });
  await sleep(1200);
  ok('pause fired', !!at('/lp/pause'));
  ok('marker held while paused', !at('/lp/t2'));
  const resumeAt = Date.now();
  await rest('/api/project/items/ev-a/resume', { method: 'POST', body: '{}' });
  await sleep(800);
  ok('resume fired', !!at('/lp/resume'));
  const t2 = at('/lp/t2');
  ok('marker fires after resume', !!t2, t2 ? `+${t2.t - resumeAt} ms after resume` : 'missing');
  await rest('/api/project/items/ev-a/stop', { method: 'POST', body: JSON.stringify({ fade_ms: 0 }) });
  await sleep(300);
  ok('end fired on stop', osc.filter(m => m.address === '/lp/end').length === 1);

  // ---- test button with an unsaved connection ----
  osc.length = 0;
  r = await rest('/api/events/test', { method: 'POST', body: JSON.stringify({
    event: { plugin: 'osc', action: 'message', options: { address: '/lp/test', args: [] } },
    connection: { id: 'draft', plugin: 'osc', config: { host: '127.0.0.1', port: oscPort } },
  }) });
  await sleep(200);
  ok('test with draft connection', r.body.ok === true && !!at('/lp/test'), JSON.stringify(r.body));
  r = await rest('/api/events/test', { method: 'POST', body: JSON.stringify({
    event: { plugin: 'osc', action: 'message', connectionId: 'nope', options: { address: '/x' } },
  }) });
  ok('test reports a missing connection', r.body.ok === false && /connection/.test(r.body.error || ''),
     JSON.stringify(r.body));

  // ---- connections survive a save that doesn't carry them ----
  // (Three by now: the migration below added one for the legacy HTTP action.)
  r = await rest('/api/project');
  const full = r.body.project || r.body;
  const before = (full.eventConnections || []).length;
  delete full.eventConnections;
  await rest('/api/project/document', { method: 'PUT', body: JSON.stringify(full) });
  r = await rest('/api/events/connections');
  const after = (r.body.connections || []).length;
  ok('connections kept across a save without them', before >= 2 && after === before,
     `${before} before, ${after} after`);

  // ---- migration ----
  r = await rest('/api/project');
  const proj = r.body.project || r.body;
  const legacy = (proj.items || []).find(i => i.uuid === 'ev-legacy') || {};
  const evs = legacy.events || [];
  const conns = proj.eventConnections || [];
  const httpEv = evs.find(e => e.plugin === 'http');
  const migratedConn = conns.find(c => c.id === httpEv?.connectionId);
  ok('customActions removed', legacy.customActions === undefined);
  ok('customActions migrated', evs.length === 2 &&
     evs.some(e => e.plugin === 'liveplay' && e.action === 'stop-all' && e.time === 1.5) &&
     httpEv && httpEv.options.path === '/api/go' && httpEv.options.body === '{"a":1}' &&
     migratedConn && migratedConn.config.baseUrl === 'http://10.0.0.9:8080',
     JSON.stringify(evs));

  udp.close(); srv.close();
  console.log(failures ? `\n${failures} FAILED` : '\nALL PASSED');
  process.exit(failures ? 1 : 0);
})().catch(e => { console.error(e); process.exit(2); });

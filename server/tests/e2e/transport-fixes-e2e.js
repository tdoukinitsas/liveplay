// Transport fixes for 2.5.0, end-to-end against a running server:
//
//   A. #62  CORS preflight. Crow answers every OPTIONS itself, before any
//           route could run, so the headers must come from middleware. Checked
//           on a route with explicit GET+POST (/api/master/gain, the one the
//           report named), on a PATCH route, and on Crow's own 404.
//   B. #65  POST /api/cues/<id>/play goes through the item: a second play
//           after a natural end starts again at the in-point, inside the trim,
//           instead of resuming at the out-point / EOF.
//   C. #56  Manual stop fade: manualStopFade governs the Stop button, 0 cuts;
//           a stop's fade_ms overrides it; an item without the field keeps the
//           2.4 rule (max of stopFade and fadeOutDuration).
//   D. #60  Preview: Stop All clears the preview state and says so; so does an
//           audition that plays to its end; stopAllStopsPreview:false spares it.
//
// Generates its own short signals; needs an audio device (a Stopped/Playing
// transition only happens while the render thread is running).
//
// usage: node transport-fixes-e2e.js <port>
const fs   = require('fs');
const os   = require('os');
const path = require('path');
const WebSocket = require(require.resolve('ws', { paths: [process.cwd()] }));

const PORT = process.argv[2] || '4480';
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
  return { status: r.status, body, headers: r.headers };
}
const post = (p, body = {}) => rest(p, { method: 'POST', body: JSON.stringify(body) });

// Transport enum, audio/playback_item.hpp.
const T = { Stopped: 0, Playing: 1, FadingIn: 2, FadingOut: 3, Paused: 4 };

// 48 kHz 16-bit stereo, a quiet 440 Hz tone. Quiet so nothing trips a limiter.
function writeWav(file, secs) {
  const rate = 48000, ch = 2, n = Math.round(rate * secs);
  const data = Buffer.alloc(n * ch * 2);
  for (let i = 0; i < n; i++) {
    const v = Math.round(Math.sin(2 * Math.PI * 440 * i / rate) * 0.1 * 32767);
    data.writeInt16LE(v, i * 4); data.writeInt16LE(v, i * 4 + 2);
  }
  const h = Buffer.alloc(44);
  h.write('RIFF', 0); h.writeUInt32LE(36 + data.length, 4); h.write('WAVE', 8);
  h.write('fmt ', 12); h.writeUInt32LE(16, 16); h.writeUInt16LE(1, 20); h.writeUInt16LE(ch, 22);
  h.writeUInt32LE(rate, 24); h.writeUInt32LE(rate * ch * 2, 28); h.writeUInt16LE(ch * 2, 32);
  h.writeUInt16LE(16, 34); h.write('data', 36); h.writeUInt32LE(data.length, 40);
  fs.writeFileSync(file, Buffer.concat([h, data]));
}

async function cueFor(itemUuid) {
  // The item -> engine cue mapping is in the playback snapshot's cue list via
  // WS, but GET /api/cues is enough: each cue's file is unique in this test.
  const cues = (await rest('/api/cues')).body;
  const items = (await rest('/api/project/items')).body;
  const flat = [];
  const walk = xs => xs.forEach(x => { flat.push(x); if (x.children) walk(x.children); });
  walk(Array.isArray(items) ? items : (items.items || []));
  const item = flat.find(i => i.uuid === itemUuid);
  const want = path.resolve(item.mediaServerPath).toLowerCase();
  return cues.find(c => path.resolve(c.file_path).toLowerCase() === want);
}
const cueState = async id => (await rest(`/api/cues/${id}`)).body;
async function waitFor(pred, ms = 5000, step = 50) {
  const end = Date.now() + ms;
  while (Date.now() < end) { if (await pred()) return true; await sleep(step); }
  return false;
}

(async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'lp-transport-'));
  const long  = path.join(dir, 'long.wav');   writeWav(long, 20);
  const short = path.join(dir, 'short.wav');  writeWav(short, 1.5);
  const trim  = path.join(dir, 'trim.wav');   writeWav(trim, 6);
  const legacy = path.join(dir, 'legacy.wav'); writeWav(legacy, 20);
  const cut   = path.join(dir, 'cut.wav');    writeWav(cut, 20);

  const items = [
    // Trimmed 1.0 -> 2.0 s, no fades, stops at the out-point.
    { uuid: 'it-trim',   type: 'audio', displayName: 'Trim',   mediaServerPath: trim,
      inPoint: 1.0, outPoint: 2.0, fadeOutDuration: 0, endBehavior: { action: 'nothing' } },
    // Explicit 2 s manual stop fade.
    { uuid: 'it-long',   type: 'audio', displayName: 'Long',   mediaServerPath: long,
      manualStopFade: 2.0, fadeOutDuration: 1.0, endBehavior: { action: 'nothing' } },
    // manualStopFade 0 = the Stop button cuts.
    { uuid: 'it-cut',    type: 'audio', displayName: 'Cut',    mediaServerPath: cut,
      manualStopFade: 0, fadeOutDuration: 1.0, endBehavior: { action: 'nothing' } },
    // A 2.4 item: no manualStopFade, stopFade 1.5 -> Stop fades 1.5 s.
    { uuid: 'it-legacy', type: 'audio', displayName: 'Legacy', mediaServerPath: legacy,
      stopFade: 1.5, fadeOutDuration: 1.0, endBehavior: { action: 'nothing' } },
    { uuid: 'it-short',  type: 'audio', displayName: 'Short',  mediaServerPath: short,
      fadeOutDuration: 0, endBehavior: { action: 'nothing' } },
  ];
  let r = await rest('/api/project/document', {
    method: 'PUT', body: JSON.stringify({ name: 'transport-fixes-e2e', items }),
  });
  ok('loaded the test project', r.status === 200, `status ${r.status}`);
  await waitFor(async () => (await rest('/api/project/progress')).body.loading === false, 10000, 100);

  // ---- A. CORS preflight (#62) -------------------------------------------
  const pre = (p, method) => fetch(BASE + p, { method: 'OPTIONS', headers: {
    Origin: 'http://example.test', 'Access-Control-Request-Method': method,
    'Access-Control-Request-Headers': 'content-type' } });
  let o = await pre('/api/master/gain', 'POST');
  ok('preflight on an explicit GET+POST route carries Allow-Origin',
     !!o.headers.get('access-control-allow-origin'),
     `${o.status} ACAO=${o.headers.get('access-control-allow-origin')}`);
  ok('preflight allows POST and the Authorization header',
     /POST/.test(o.headers.get('access-control-allow-methods') || '') &&
     /authorization/i.test(o.headers.get('access-control-allow-headers') || ''),
     `methods=${o.headers.get('access-control-allow-methods')} headers=${o.headers.get('access-control-allow-headers')}`);
  o = await pre('/api/project/items/it-long', 'PATCH');
  ok('preflight allows PATCH', /PATCH/.test(o.headers.get('access-control-allow-methods') || ''),
     `methods=${o.headers.get('access-control-allow-methods')}`);
  const nf = await rest('/api/definitely-not-a-route');
  ok("Crow's own 404 carries Allow-Origin too", !!nf.headers.get('access-control-allow-origin'),
     `${nf.status} ACAO=${nf.headers.get('access-control-allow-origin')}`);

  // ---- B. REST cue play restarts at the in-point (#65) ---------------------
  const trimCue = await cueFor('it-trim');
  ok('the trimmed item has an engine cue', !!trimCue, trimCue ? trimCue.id : 'none');
  r = await post(`/api/cues/${trimCue.id}/play`);
  ok('POST /api/cues/<id>/play succeeds', r.status === 200, `status ${r.status}`);
  await sleep(250);
  let s = await cueState(trimCue.id);
  ok('first play starts inside the trim', s.playhead_seconds >= 1.0 && s.playhead_seconds < 2.0,
     `playhead ${s.playhead_seconds.toFixed(2)} s`);
  const ended = await waitFor(async () => (await cueState(trimCue.id)).transport === T.Stopped, 4000);
  ok('it stops at the out-point', ended, `transport ${(await cueState(trimCue.id)).transport}`);
  await post(`/api/cues/${trimCue.id}/play`);
  await sleep(250);
  s = await cueState(trimCue.id);
  ok('a second play after the end starts again at the in-point',
     s.transport !== T.Stopped && s.playhead_seconds >= 1.0 && s.playhead_seconds < 1.6,
     `transport ${s.transport}, playhead ${s.playhead_seconds.toFixed(2)} s`);
  await post(`/api/cues/${trimCue.id}/stop`, { fade_ms: 0 });
  await sleep(100);
  ok('a stop with fade_ms 0 via /api/cues cuts', (await cueState(trimCue.id)).transport === T.Stopped,
     `transport ${(await cueState(trimCue.id)).transport}`);
  await post(`/api/cues/${trimCue.id}/play`);
  await sleep(250);
  s = await cueState(trimCue.id);
  ok('play after a stop starts again at the in-point, not frame 0',
     s.playhead_seconds >= 1.0 && s.playhead_seconds < 1.6, `playhead ${s.playhead_seconds.toFixed(2)} s`);
  await post('/api/transport/stop_all', { fade_ms: 0 });

  // ---- C. Manual stop fade (#56) ------------------------------------------
  const stopAfter = async (uuid, body) => {
    await post(`/api/project/items/${uuid}/play`);
    await sleep(400);
    const c = await cueFor(uuid);
    await post(`/api/project/items/${uuid}/stop`, body);
    await sleep(300);
    return (await cueState(c.id)).transport;
  };
  let t = await stopAfter('it-long', {});
  ok('manualStopFade 2 s: 300 ms after Stop it is still fading', t === T.FadingOut, `transport ${t}`);
  await post('/api/transport/stop_all', { fade_ms: 0 });
  t = await stopAfter('it-cut', {});
  ok('manualStopFade 0: Stop cuts', t === T.Stopped, `transport ${t}`);
  t = await stopAfter('it-long', { fade_ms: 0 });
  ok("a stop's fade_ms 0 overrides the item's 2 s fade", t === T.Stopped, `transport ${t}`);
  t = await stopAfter('it-cut', { fade_ms: 3000 });
  ok("a stop's fade_ms 3000 overrides the item's cut", t === T.FadingOut, `transport ${t}`);
  await post('/api/transport/stop_all', { fade_ms: 0 });
  t = await stopAfter('it-legacy', {});
  ok('an item without manualStopFade keeps the 2.4 rule (stopFade 1.5 s)', t === T.FadingOut, `transport ${t}`);
  await post('/api/transport/stop_all', { fade_ms: 0 });

  // ---- D. Preview state (#60) ---------------------------------------------
  const ws = new WebSocket(`ws://127.0.0.1:${PORT}/ws`);
  await new Promise((res, rej) => { ws.once('open', res); ws.once('error', rej); });
  let stoppedFrames = 0;
  ws.on('message', raw => {
    let m; try { m = JSON.parse(raw); } catch { return; }
    if (m.type === 'doc_patch' && m.op === 'preview_stopped') stoppedFrames++;
  });
  const previewActive = async () => (await rest('/api/preview')).body.active;

  r = await post('/api/preview', { itemUuid: 'it-long' });
  ok('a preview starts', r.status === 200 && await previewActive(), `status ${r.status}`);
  stoppedFrames = 0;
  await post('/api/transport/stop_all', { fade_ms: 300 });
  await sleep(200);
  ok('Stop All clears the preview state', !(await previewActive()), 'GET /api/preview');
  ok('Stop All broadcasts preview_stopped', stoppedFrames === 1, `${stoppedFrames} frame(s)`);

  r = await post('/api/preview', { itemUuid: 'it-short' });
  stoppedFrames = 0;
  const cleared = await waitFor(async () => !(await previewActive()), 4000);
  ok('a preview that plays to its end clears itself', cleared, 'GET /api/preview');
  ok('...and broadcasts preview_stopped', stoppedFrames === 1, `${stoppedFrames} frame(s)`);

  await rest('/api/project/settings', { method: 'PATCH', body: JSON.stringify({ stopAllStopsPreview: false }) });
  await post('/api/preview', { itemUuid: 'it-long' });
  stoppedFrames = 0;
  await post('/api/transport/stop_all', { fade_ms: 0 });
  await sleep(400);
  ok('stopAllStopsPreview:false leaves the audition running', await previewActive(),
     `${stoppedFrames} preview_stopped frame(s)`);
  await rest('/api/preview', { method: 'DELETE' });
  await sleep(200);
  ok('DELETE /api/preview still broadcasts exactly once', stoppedFrames === 1, `${stoppedFrames} frame(s)`);
  await rest('/api/project/settings', { method: 'PATCH', body: JSON.stringify({ stopAllStopsPreview: true }) });

  ws.close();
  await rest('/api/project/close', { method: 'POST', body: '{}' });
  fs.rmSync(dir, { recursive: true, force: true });
  console.log(failures ? `\nFAILURES: ${failures}` : '\nALL PASS');
  process.exit(failures ? 1 : 0);
})().catch(e => { console.error(e); process.exit(2); });

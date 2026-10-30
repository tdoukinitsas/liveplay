// The LivePlay event plugin (#84) against a running server: cues and the
// internal mixer, driven through POST /api/events/test and read back through
// the REST API and the meters. No device needed; plays a quiet tone.
//
// usage: node liveplay-plugin-e2e.js <port>
const fs = require('fs');
const os = require('os');
const path = require('path');
const WebSocket = require(require.resolve('ws', { paths: [process.cwd()] }));

const PORT = process.argv[2] || '4480';
const BASE = `http://127.0.0.1:${PORT}`;
let failures = 0;
const ok = (name, pass, detail = '') => { console.log(`${pass ? 'PASS' : 'FAIL'}  ${name}   ${detail}`); if (!pass) failures++; };
const sleep = ms => new Promise(r => setTimeout(r, ms));
async function rest(p, opts = {}) {
  const r = await fetch(BASE + p, { headers: { 'content-type': 'application/json' }, ...opts });
  const text = await r.text();
  let body; try { body = JSON.parse(text); } catch { body = text; }
  return { status: r.status, body };
}
const fire = async (action, options) => (await rest('/api/events/test', { method: 'POST',
  body: JSON.stringify({ event: { plugin: 'liveplay', action, options } }) })).body;
const options = async (source, values = {}) => (await rest('/api/events/options', { method: 'POST',
  body: JSON.stringify({ plugin: 'liveplay', source, values }) })).body;
const bus = async id => (await rest('/api/buses')).body.find(b => b.id === id);
const cue = async uuid => {
  const items = (await rest('/api/project/items?offset=0&limit=10')).body.items;
  const cueId = items.find(i => i.uuid === uuid)?.cueId;
  return (await rest('/api/cues')).body.find(c => c.id === cueId);
};

function writeWav(file, seconds) {
  const rate = 48000, n = rate * seconds, data = Buffer.alloc(n * 4);
  for (let i = 0; i < n; i++) {
    const v = Math.round(Math.sin(2 * Math.PI * 440 * i / rate) * 0.1 * 32767);
    data.writeInt16LE(v, i * 4); data.writeInt16LE(v, i * 4 + 2);
  }
  const h = Buffer.alloc(44);
  h.write('RIFF', 0); h.writeUInt32LE(36 + data.length, 4); h.write('WAVE', 8); h.write('fmt ', 12);
  h.writeUInt32LE(16, 16); h.writeUInt16LE(1, 20); h.writeUInt16LE(2, 22); h.writeUInt32LE(rate, 24);
  h.writeUInt32LE(rate * 4, 28); h.writeUInt16LE(4, 32); h.writeUInt16LE(16, 34); h.write('data', 36);
  h.writeUInt32LE(data.length, 40);
  fs.writeFileSync(file, Buffer.concat([h, data]));
}

// Average source peak of one cue over `ms`, from the meter broadcast.
async function peak(ws, cueId, ms) {
  const vals = [];
  const on = raw => {
    let m; try { m = JSON.parse(raw); } catch { return; }
    if (m.type !== 'meters') return;
    for (const it of m.items || []) if (it.cue_id === cueId && it.sources)
      vals.push(Math.max(...it.sources.map(s => s.peak_max_db ?? s.peak_db ?? -200)));
  };
  ws.on('message', on); await sleep(ms); ws.off('message', on);
  return vals.length ? vals.reduce((a, b) => a + b, 0) / vals.length : NaN;
}

(async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'lp-lpplug-'));
  const wav = path.join(dir, 'tone.wav'); writeWav(wav, 20);
  await rest('/api/project/document', { method: 'PUT', body: JSON.stringify({ name: 'liveplay-plugin-e2e',
    items: [
      { uuid: 'a', type: 'audio', displayName: 'Tone A', mediaServerPath: wav, volume: 1, manualStopFade: 0, endBehavior: { action: 'nothing' } },
      { uuid: 'b', type: 'audio', displayName: 'Tone B', mediaServerPath: wav, volume: 1, manualStopFade: 0, endBehavior: { action: 'nothing' } },
    ] }) });
  await sleep(1500);
  const ws = new WebSocket(`ws://127.0.0.1:${PORT}/ws`);
  await new Promise(r => ws.on('open', r));

  // ---- Option lists ----
  let o = await options('buses');
  const master = (await rest('/api/buses')).body.find(b => b.master);
  ok('buses listed', (o.options || []).some(x => x.value === master.id && /master/.test(x.label)), JSON.stringify(o.options));
  o = await options('bus-routes');
  ok('routes listed', (o.options || []).some(x => x.value === 'none') && (o.options || []).some(x => x.value.startsWith('output:')));

  // ---- Cues ----
  let r = await fire('play-item', { item: 'a' }); await sleep(600);
  ok('play cue', r.ok && (await cue('a'))?.transport === 1, JSON.stringify(r));
  const cueId = (await cue('a')).id;
  const before = await peak(ws, cueId, 500);
  r = await fire('duck-item', { item: 'a', level: -12, fade: 0 }); await sleep(300);
  const ducked = await peak(ws, cueId, 500);
  ok('duck a cue by 12 dB', r.ok && Math.abs((before - ducked) - 12) < 1.5, `${before.toFixed(1)} -> ${ducked.toFixed(1)} dB`);
  r = await fire('duck-item', { item: 'a', level: 0, fade: 1 }); await sleep(1400);
  const back = await peak(ws, cueId, 500);
  ok('fade the duck back', r.ok && Math.abs(back - before) < 1, `${back.toFixed(1)} dB`);
  r = await fire('duck-item', { item: 'b', level: -12 });
  ok('duck refuses a cue that is not playing', r.ok === false && /playing/.test(r.error), r.error);
  r = await fire('pause-item', { item: 'a' }); await sleep(400);
  ok('pause cue', r.ok && (await cue('a'))?.transport === 4);
  r = await fire('resume-item', { item: 'a' }); await sleep(400);
  ok('resume cue', r.ok && (await cue('a'))?.transport === 1);
  r = await fire('arm-next', { item: 'b' });
  ok('set Up Next', r.ok);
  r = await fire('stop-item', { item: 'a' }); await sleep(400);
  ok('stop cue', r.ok && (await cue('a'))?.transport !== 1);

  // ---- Master level ----
  const g0 = (await rest('/api/master/gain')).body.db;
  r = await fire('master-level', { level: -6, fade: 1 }); await sleep(400);
  const mid = (await rest('/api/master/gain')).body.db;
  await sleep(900);
  const g1 = (await rest('/api/master/gain')).body.db;
  ok('master level fades', r.ok && mid < g0 && mid > -6 && Math.abs(g1 + 6) < 0.01, `${g0} -> ${mid.toFixed(2)} -> ${g1}`);
  await fire('master-level', { level: g0, fade: 0 });

  // ---- Buses ----
  const mk = async name => (await rest('/api/buses', { method: 'POST', body: JSON.stringify({ name }) })).body;
  const A = await mk('E2E A'), Bb = await mk('E2E B');
  const aId = A.id ?? A.bus?.id, bId = Bb.id ?? Bb.bus?.id;
  ok('test buses created', !!aId && !!bId, `${aId} ${bId}`);

  r = await fire('bus-level', { bus: aId, level: -10, fade: 1 }); await sleep(400);
  const half = (await bus(aId)).gainDb; await sleep(900);
  const done = (await bus(aId)).gainDb;
  ok('bus fader fades, saved when it lands', r.ok && half === 0 && Math.abs(done + 10) < 0.01, `saved mid-fade ${half}, then ${done}`);
  r = await fire('bus-mute', { bus: aId, state: 'mute' });
  ok('bus mute', r.ok && (await bus(aId)).mute === true);
  await fire('bus-mute', { bus: aId, state: 'unmute' });
  ok('bus unmute', (await bus(aId)).mute === false);

  r = await fire('bus-dsp', { bus: aId, param: 'comp.threshold', value: -24, fade: 0 });
  ok('bus processing value', r.ok && (await bus(aId)).dsp?.comp?.threshold === -24, JSON.stringify((await bus(aId)).dsp?.comp));
  r = await fire('bus-dsp', { bus: aId, param: 'comp.on', value: 1 });
  ok('bus processing switch', r.ok && (await bus(aId)).dsp?.comp?.on === true);

  r = await fire('bus-route', { bus: aId, to: `bus:${bId}` });
  ok('route a bus to another', r.ok, JSON.stringify(r));
  r = await fire('bus-route', { bus: bId, to: `bus:${aId}` });
  ok('refuses a loop', r.ok === false && /itself/.test(r.error || ''), r.error);
  r = await fire('bus-route', { bus: aId, to: 'none' });
  ok('route a bus nowhere', r.ok);

  await rest(`/api/buses/${aId}`, { method: 'PATCH', body: JSON.stringify({ sends: [{ id: bId, levelDb: 0, tap: 'post' }] }) });
  o = await options('bus-sends', { bus: aId });
  ok('sends listed for the bus', (o.options || []).some(x => x.value === bId), JSON.stringify(o.options));
  r = await fire('bus-send', { bus: aId, send: bId, level: -20, fade: 0.5 }); await sleep(900);
  const doc = (await rest('/api/project')).body;
  const sendNow = (doc.buses || []).find(b => b.id === aId)?.sends?.[0]?.levelDb;
  ok('send level fades to its target', r.ok && Math.abs(sendNow + 20) < 0.01, String(sendNow));
  r = await fire('bus-send', { bus: bId, send: aId, level: -5 });
  ok('refuses a send that does not exist', r.ok === false, r.error);

  await rest(`/api/buses/${aId}`, { method: 'DELETE' });
  await rest(`/api/buses/${bId}`, { method: 'DELETE' });
  ws.close();
  console.log(failures ? `\n${failures} FAILED` : '\nALL PASSED');
  process.exit(failures ? 1 : 0);
})().catch(e => { console.error(e); process.exit(2); });

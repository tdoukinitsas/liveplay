// Language and meter rate belong to the person at the surface, not to the rig
// (U2).
//
// `ui_locale_` was one string on the server. A `set_locale` from any client
// wrote it and broadcast `locale_changed` to everyone, so one operator
// switching to Greek switched every other client — and every control surface —
// with them. A presentation preference imposed across users is exactly the leak
// rule R2 exists to catch, and it was the spec's nominated first test of the
// User tier.
//
// What this pins:
//   • one client's locale change reaches that client and NOBODY else;
//   • the installation default still exists and still travels — but only to
//     sessions that have expressed no preference, so a client that chose for
//     itself is not dragged back by the house changing its default;
//   • /api/clients reports each session's effective locale and rate, and
//     whether it is the session's own or inherited;
//   • a client asking for fewer meter frames measurably gets fewer, while one
//     that asked for nothing is unaffected;
//   • a rate above what the server ticks at is clamped, and the client is told
//     what it actually got rather than left to assume;
//   • cue_state EDGES are never thinned. This is the one that matters: meters
//     are samples and dropping one costs resolution, but dropping a transition
//     costs the client a fact it will never be told again, and its transport
//     display stays wrong until something else moves.
//
// usage: node session-prefs-e2e.js <port> <wavPath>
const WebSocket = require(require.resolve('ws', { paths: [process.cwd()] }));

const PORT = process.argv[2] || '4500';
const WAV  = process.argv[3];
const BASE = `http://127.0.0.1:${PORT}`;

let failures = 0;
const ok = (name, pass, detail) => {
  if (detail === undefined) detail = '(no detail given — fix the assertion)';
  console.log(`${pass ? 'PASS' : 'FAIL'}  ${name}   ${detail}`);
  if (!pass) failures++;
};
const sleep = ms => new Promise(r => setTimeout(r, ms));

async function rest(path, opts = {}) {
  const r = await fetch(BASE + path, { headers: { 'content-type': 'application/json' }, ...opts });
  const t = await r.text();
  let body; try { body = JSON.parse(t); } catch { body = t; }
  return { status: r.status, body };
}
const clients = async () => {
  const r = await rest('/api/clients');
  return Array.isArray(r.body) ? r.body : [];
};

// One socket, recording everything it is sent so the assertions can ask what
// this client — and only this client — heard.
class Client {
  constructor(ws) {
    this.ws = ws;
    this.frames = [];
    ws.on('message', raw => {
      let m; try { m = JSON.parse(raw); } catch { return; }
      this.frames.push(m);
    });
  }
  reset() { this.frames = []; }
  meters()  { return this.frames.filter(f => f.type === 'meters').length; }
  patches(op) { return this.frames.filter(f => f.type === 'doc_patch' && f.op === op); }
  cueStates() { return this.frames.filter(f => f.type === 'cue_state'); }
  send(o) { this.ws.send(JSON.stringify(o)); }
  close() { try { this.ws.close(); } catch { /* already gone */ } }
}
function open(port) {
  return new Promise((res, rej) => {
    const ws = new WebSocket(`ws://127.0.0.1:${port}/ws`);
    ws.on('open',  () => res(new Client(ws)));
    ws.on('error', rej);
  });
}

// Which /api/clients row belongs to this socket? The server does not tell a
// client its own id, and connect order is not registration order, so each is
// identified by connecting one at a time and taking the row that is new.
async function idOfNewest(known) {
  const rows = await clients();
  const fresh = rows.filter(r => !known.has(r.id));
  return fresh.length === 1 ? fresh[0].id : -1;
}

const play = uuid => rest(`/api/project/items/${uuid}/play`, { method: 'POST', body: '{}' });
const stop = uuid => rest(`/api/project/items/${uuid}/stop`, { method: 'POST', body: '{}' });

(async () => {
  let A, B;
  const originalLocale = (await rest('/api/ui/locale')).body?.locale ?? 'en';

  try {
    const known = new Set();
    A = await open(PORT);
    await sleep(500);
    const idA = await idOfNewest(known);
    known.add(idA);
    B = await open(PORT);
    await sleep(500);
    const idB = await idOfNewest(known);
    ok('two clients connect and are told apart',
       idA > 0 && idB > 0 && idA !== idB, `A #${idA}, B #${idB}`);

    // ---- 1. A locale change reaches its own client and nobody else -------
    A.reset(); B.reset();
    A.send({ type: 'set_locale', locale: 'el' });
    await sleep(600);
    const aHeard = A.patches('locale_changed');
    const bHeard = B.patches('locale_changed');
    ok('the client that changed its language is told so',
       aHeard.length >= 1 && aHeard[aHeard.length - 1].locale === 'el',
       `A heard ${JSON.stringify(aHeard.map(p => p.locale))}`);
    ok('and the OTHER client hears nothing at all',
       bHeard.length === 0, `B heard ${JSON.stringify(bHeard.map(p => p.locale))}`);

    let rows = await clients();
    const rowA = rows.find(r => r.id === idA);
    const rowB = rows.find(r => r.id === idB);
    ok('the session records the language it chose',
       rowA?.locale === 'el' && rowA?.localeIsOwn === true,
       `A locale ${rowA?.locale} own=${rowA?.localeIsOwn}`);
    ok('...and the other session still inherits the default',
       rowB?.locale === originalLocale && rowB?.localeIsOwn === false,
       `B locale ${rowB?.locale} own=${rowB?.localeIsOwn}`);

    // ---- 2. The default still travels, but not over a chosen one ---------
    A.reset(); B.reset();
    const r = await rest('/api/ui/locale', {
      method: 'POST', body: JSON.stringify({ locale: 'fr' }),
    });
    ok('the installation default can be set', r.status === 200 && r.body?.locale === 'fr',
       `status ${r.status}, locale ${r.body?.locale}`);
    await sleep(600);
    ok('a session with no preference of its own follows the default',
       B.patches('locale_changed').some(p => p.locale === 'fr'),
       `B heard ${JSON.stringify(B.patches('locale_changed').map(p => p.locale))}`);
    ok('...and a session that chose for itself is NOT dragged back',
       A.patches('locale_changed').length === 0,
       `A heard ${JSON.stringify(A.patches('locale_changed').map(p => p.locale))}`);

    rows = await clients();
    ok('the chosen language survives the default changing underneath it',
       rows.find(x => x.id === idA)?.locale === 'el',
       `A locale ${rows.find(x => x.id === idA)?.locale}`);

    // ---- 3. Meter rate is per connection ---------------------------------
    // Baseline first: without it, "A got fewer" would also pass if A's socket
    // were simply dead.
    A.reset(); B.reset();
    await sleep(2000);
    const baseA = A.meters(), baseB = B.meters();
    ok('both clients receive meters at the same rate to begin with',
       baseA > 10 && baseB > 10 && Math.abs(baseA - baseB) <= Math.max(4, baseA * 0.35),
       `A ${baseA} frames, B ${baseB} frames`);

    A.reset(); B.reset();
    A.send({ type: 'set_meter_hz', hz: 5 });
    await sleep(400);
    const rateReply = A.patches('meter_hz_changed');
    ok('the client is told the rate it actually got',
       rateReply.length >= 1 && rateReply[rateReply.length - 1].hz === 5,
       `replied ${JSON.stringify(rateReply.map(p => p.hz))}`);

    A.reset(); B.reset();
    await sleep(2000);
    const slowA = A.meters(), fullB = B.meters();
    ok('the client that asked for fewer meter frames gets fewer',
       slowA > 0 && slowA < fullB / 2,
       `A ${slowA} frames vs B ${fullB} over 2 s`);
    ok('...and the client that asked for nothing is unaffected',
       Math.abs(fullB - baseB) <= Math.max(5, baseB * 0.35),
       `B ${baseB} → ${fullB} frames`);

    rows = await clients();
    const slowRow = rows.find(x => x.id === idA);
    ok('the session records the rate it asked for',
       slowRow?.meterHz === 5 && slowRow?.meterHzIsOwn === true,
       `A meterHz ${slowRow?.meterHz} own=${slowRow?.meterHzIsOwn}`);

    // ---- 4. A rate above what the server ticks at is clamped -------------
    // The loop is the only consumer of the consuming meter reads, so a faster
    // request cannot be honoured by anybody — and the client should be told.
    A.reset();
    A.send({ type: 'set_meter_hz', hz: 1000 });
    await sleep(400);
    const clamped = A.patches('meter_hz_changed');
    const serverHz = rows.find(x => x.id === idB)?.meterHz;
    ok('asking for more than the server ticks at is clamped, and said',
       clamped.length >= 1 && clamped[clamped.length - 1].hz === serverHz,
       `asked 1000, told ${clamped[clamped.length - 1]?.hz}, server ticks at ${serverHz}`);

    // ---- 5. Edges are never thinned --------------------------------------
    // The load-bearing distinction. A client down at 1 Hz still has to hear
    // every transport transition, or its display stays wrong until something
    // else happens to move.
    if (!WAV) {
      console.log('SKIP  no wav given — cannot check that cue_state survives thinning');
    } else {
      await rest('/api/project/document', {
        method: 'PUT',
        body: JSON.stringify({
          name: 'session-prefs-e2e', version: '2.0.0', busSchema: 2,
          items: [{ uuid: 'sp-item', type: 'audio', displayName: 'Probe',
                    mediaServerPath: WAV, volume: 1, endBehavior: 'loop' }],
        }),
      });
      for (let i = 0; i < 60; i++) {
        const p = await rest('/api/project/progress');
        if (p.body && p.body.loading === false) break;
        await sleep(100);
      }
      A.reset();
      A.send({ type: 'set_meter_hz', hz: 1 });
      await sleep(400);

      A.reset();
      await play('sp-item');
      await sleep(1200);
      await stop('sp-item');
      await sleep(1200);

      const states = A.cueStates();
      ok('a client thinned to 1 Hz still hears every transport edge',
         states.length >= 2,
         `${states.length} cue_state frame(s): ` +
         `${JSON.stringify(states.map(s => s.transport))}`);
      ok('...while still receiving far fewer meter frames than the tick rate',
         A.meters() < 20, `${A.meters()} meter frames over ~2.4 s`);
    }
  } finally {
    // Leave the installation default as it was — other suites share this
    // server, and a locale left in French is a booby trap for the next one.
    try {
      await rest('/api/ui/locale', {
        method: 'POST', body: JSON.stringify({ locale: originalLocale }),
      });
    } catch { /* best effort */ }
    if (A) A.close();
    if (B) B.close();
    await sleep(200);
  }

  console.log(`\n${failures === 0 ? 'ALL PASS' : 'FAILURES'} (${failures})`);
  process.exit(failures === 0 ? 0 : 1);
})().catch(e => { console.error('harness error:', e); process.exit(2); });

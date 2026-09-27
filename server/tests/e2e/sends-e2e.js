// M1: aux sends e2e. Real audio, real render loop, against a running server.
//
// A send is a tapped COPY of a bus into another bus, at its own level, taken
// either before or after that bus's fader. It is not the bus's output — a bus
// keeps exactly one of those (D5) and may have any number of sends.
//
// THE CLAIM THIS SUITE EXISTS FOR is assertion 4: a PRE-fader send does not
// follow the source's fader, and a POST-fader one does. Everything else here
// could be got right by an implementation that ignored the tap point entirely;
// that one cannot. It is also the claim an operator's foldback depends on —
// pull the house fader at the end of a number and the wedges must not die with
// it — so it is checked with a real fader move and real meters, not by reading
// back what was stored.
//
// usage: node sends-e2e.js <port> <wavPath>
const WebSocket = require(require.resolve('ws', { paths: [process.cwd()] }));

const PORT = process.argv[2] || '4580';
const WAV  = process.argv[3];
const BASE = `http://127.0.0.1:${PORT}`;

let failures = 0;
const ok = (name, pass, detail = '') => {
  console.log(`${pass ? 'PASS' : 'FAIL'}  ${name}${detail ? '   ' + detail : ''}`);
  if (!pass) failures++;
};
const sleep = ms => new Promise(r => setTimeout(r, ms));

async function rest(path, opts = {}) {
  const r = await fetch(BASE + path, {
    headers: { 'content-type': 'application/json' },
    ...opts,
  });
  const text = await r.text();
  let body; try { body = JSON.parse(text); } catch { body = text; }
  return { status: r.status, body };
}
const buses = async () => (await rest('/api/buses')).body;
const bus   = async (id) => (await buses()).find(b => b.id === id);

class Meters {
  constructor(ws) {
    this.frames = [];
    ws.on('message', raw => {
      let m; try { m = JSON.parse(raw); } catch { return; }
      if (m.type === 'meters') this.frames.push(m);
    });
  }
  reset() { this.frames = []; }
  peak(mixerId) {
    let best = -200;
    for (const f of this.frames)
      for (const c of (f.mixer_channels || []))
        if (c.mixer_id === mixerId) best = Math.max(best, c.peak_db ?? -200);
    return best;
  }
}
async function measure(m, ms) { m.reset(); await sleep(ms); }

// How long to let the METERS settle after a level change before opening a
// measurement window. The strip meters have ballistics — they decay to a new,
// lower level rather than stepping to it — so a window opened too soon catches
// the tail on its way down and reports a smaller drop than actually happened.
// A first pass at this suite used 400-500 ms and read a -12 dB send as -8.2 dB
// and a -18 dB fader as -11.3 dB, both of which looked like routing bugs and
// were the decay curve.
const SETTLE = 1600;
// Mute has furthest to fall, so it gets longer before silence is asserted.
const SETTLE_SILENCE = 2500;

async function waitLoaded() {
  for (let i = 0; i < 80; i++) {
    const p = await rest('/api/project/progress');
    if (p.body && p.body.loading === false) return;
    await sleep(100);
  }
}
const play = uuid => rest(`/api/project/items/${uuid}/play`, { method: 'POST', body: '{}' });
const stop = uuid => rest(`/api/project/items/${uuid}/stop`, { method: 'POST', body: '{}' });
const setBus = (uuid, busId) =>
  rest(`/api/project/items/${uuid}`, { method: 'PATCH', body: JSON.stringify({ busId }) });
const patchBus = (id, patch) =>
  rest(`/api/buses/${id}`, { method: 'PATCH', body: JSON.stringify(patch) });
const setSends = (id, sends) => patchBus(id, { sends });

(async () => {
  const ws = new WebSocket(`ws://127.0.0.1:${PORT}/ws`);
  await new Promise((res, rej) => { ws.on('open', res); ws.on('error', rej); });
  const m = new Meters(ws);

  // A fresh document, so every id below is created from scratch and nothing
  // carries over from a previous run — the same reasoning busbus-e2e gives.
  await rest('/api/project/document', {
    method: 'PUT',
    body: JSON.stringify({
      name: 'sends-e2e',
      items: [
        { uuid: 'item-s', type: 'audio', displayName: 'SendSig', mediaServerPath: WAV, volume: 1, endBehavior: 'loop' },
      ],
    }),
  });
  await waitLoaded();

  // SRC carries the signal and its OUTPUT goes to the master bus, as any bus
  // does. POST and PRE receive nothing but a send each, so whatever their
  // meters read arrived by the send and by nothing else — which is what makes
  // the numbers below attributable.
  const mk = async (name, width = 2) =>
    (await rest('/api/buses', { method: 'POST', body: JSON.stringify({ name, width }) })).body.id;
  const SRC  = await mk('Src');
  const POST = await mk('Post');
  const PRE  = await mk('Pre');
  const SPARE = await mk('Spare');
  await setBus('item-s', SRC);

  const mixerIdOf = async id => (await bus(id)).mixerId;
  const [mSrc, mPost, mPre, mSpare] =
    await Promise.all([SRC, POST, PRE, SPARE].map(mixerIdOf));

  const previewId = (await buses()).find(b => b.preview).id;
  const masterId  = (await buses()).find(b => b.master).id;

  // =========================================================================
  // 1. A send reaches its destination, and the output still reaches the house.
  // =========================================================================
  // A send is ADDITIONAL. If implementing it had quietly replaced the output
  // edge — the single dst_strip this feature had to grow past — SRC would
  // still be audible on POST and the house would have gone silent.
  let r = await setSends(SRC, [{ id: POST, levelDb: 0, tap: 'post' }]);
  ok('a send is accepted', r.status === 200, `HTTP ${r.status}`);

  await play('item-s');
  await sleep(500);
  await measure(m, 900);
  const srcLvl   = m.peak(mSrc);
  const postUnity = m.peak(mPost);
  ok('the source bus is audible', srcLvl > -20, `${srcLvl.toFixed(1)} dBFS`);
  ok('a post-fader send at 0 dB arrives at the destination',
     postUnity > -20, `${postUnity.toFixed(1)} dBFS`);
  ok('the send does not replace the output: the spare bus stays silent',
     m.peak(mSpare) < -60, `${m.peak(mSpare).toFixed(1)} dBFS`);

  // =========================================================================
  // 2. The send carries its own level.
  // =========================================================================
  await setSends(SRC, [{ id: POST, levelDb: -12, tap: 'post' }]);
  await sleep(SETTLE);
  await measure(m, 900);
  const postCut = m.peak(mPost);
  const lvlDrop = postUnity - postCut;
  ok('a send at -12 dB arrives 12 dB down', Math.abs(lvlDrop - 12) < 1.5,
     `${postUnity.toFixed(1)} -> ${postCut.toFixed(1)} dBFS (${lvlDrop.toFixed(1)} dB)`);
  ok('the source itself is untouched by its send level',
     Math.abs(m.peak(mSrc) - srcLvl) < 1.5,
     `${srcLvl.toFixed(1)} -> ${m.peak(mSrc).toFixed(1)} dBFS`);

  // =========================================================================
  // 3 + 4. THE TAP POINT. Post follows the fader; pre does not.
  // =========================================================================
  // Both sends at 0 dB from the same source, so at a unity fader they read the
  // same and any later difference is the fader and nothing else.
  await setSends(SRC, [
    { id: POST, levelDb: 0, tap: 'post' },
    { id: PRE,  levelDb: 0, tap: 'pre'  },
  ]);
  await sleep(SETTLE);
  await measure(m, 900);
  const postFlat = m.peak(mPost);
  const preFlat  = m.peak(mPre);
  // Fan-out: one bus feeding two destinations at once. Before M1 a strip had
  // at most ONE downstream strip (D5), so this is also the assertion that the
  // single dst_strip really did become a list.
  ok('one bus feeds two sends at once',
     postFlat > -20 && preFlat > -20,
     `post ${postFlat.toFixed(1)}, pre ${preFlat.toFixed(1)} dBFS`);
  ok('at a unity fader both taps read the same',
     Math.abs(postFlat - preFlat) < 1.5,
     `post ${postFlat.toFixed(1)}, pre ${preFlat.toFixed(1)} dBFS`);

  // Now pull the SOURCE fader down 18 dB and re-measure both.
  await patchBus(SRC, { gainDb: -18 });
  await sleep(SETTLE);
  await measure(m, 900);
  const postFaded = m.peak(mPost);
  const preFaded  = m.peak(mPre);

  ok('a POST-fader send follows the source fader down',
     Math.abs((postFlat - postFaded) - 18) < 2.0,
     `${postFlat.toFixed(1)} -> ${postFaded.toFixed(1)} dBFS (${(postFlat - postFaded).toFixed(1)} dB)`);
  // The one that matters: the foldback stays up when the house comes down.
  ok('a PRE-fader send does NOT follow the source fader',
     Math.abs(preFlat - preFaded) < 2.0,
     `${preFlat.toFixed(1)} -> ${preFaded.toFixed(1)} dBFS (${(preFlat - preFaded).toFixed(1)} dB)`);

  // And mute, which sits at the same point as the fader: a muted bus must
  // still feed its pre-fader send. This is the other half of the same claim —
  // an implementation that applied the fader before the pre tap but honoured
  // mute separately would pass the assertion above and fail here.
  await patchBus(SRC, { gainDb: 0, mute: true });
  await sleep(SETTLE_SILENCE);
  await measure(m, 900);
  const preMuted  = m.peak(mPre);
  const postMuted = m.peak(mPost);
  ok('a muted bus still feeds its PRE-fader send',
     Math.abs(preFlat - preMuted) < 2.0,
     `${preFlat.toFixed(1)} -> ${preMuted.toFixed(1)} dBFS`);
  ok('a muted bus does not feed its POST-fader send',
     postMuted < -60, `${postMuted.toFixed(1)} dBFS`);
  await patchBus(SRC, { mute: false });
  await stop('item-s');
  await sleep(400);

  // =========================================================================
  // 5. The rules. Every one of these is a 409 with its own wording.
  // =========================================================================
  const refusal = res => (res.body && res.body.error) || '';

  r = await setSends(SRC, [{ id: SRC, levelDb: 0, tap: 'post' }]);
  ok('a bus cannot send to itself', r.status === 409, `HTTP ${r.status} ${refusal(r)}`);

  r = await setSends(SRC, [{ id: 'no-such-bus', levelDb: 0, tap: 'post' }]);
  ok('a send to a bus that does not exist is refused',
     r.status === 409, `HTTP ${r.status} ${refusal(r)}`);

  r = await setSends(SRC, [{ id: previewId, levelDb: 0, tap: 'post' }]);
  ok('nothing may send to the Preview bus',
     r.status === 409, `HTTP ${r.status} ${refusal(r)}`);

  r = await setSends(previewId, [{ id: SPARE, levelDb: 0, tap: 'post' }]);
  ok('the Preview bus may not send',
     r.status === 409, `HTTP ${r.status} ${refusal(r)}`);

  // The one rule sends do NOT share with outputs, settled with the user:
  // a record or broadcast feed off the house is ordinary practice.
  //
  // The destination has to LEAVE by hardware first. Every bus is created
  // pointing at the master bus, so master -> Spare while Spare still outputs
  // to master is a genuine feedback loop and refused — correctly. A real
  // record feed is an Output-kind bus for exactly this reason, so that is what
  // Spare becomes here. "Rec Out" is a logical output name this machine does
  // not map, which leaves the bus silent and is irrelevant to the rule.
  await patchBus(SPARE, { output: { type: 'output', target: 'Rec Out' } });
  r = await setSends(masterId, [{ id: SPARE, levelDb: -6, tap: 'post' }]);
  ok('the Master bus MAY send', r.status === 200, `HTTP ${r.status} ${refusal(r)}`);

  // The loop is still refused from the other end, which is what makes the
  // line above permission for the master bus to SEND rather than permission
  // to feed it back: with that send in place, pointing Spare's output at the
  // master bus closes master -> Spare -> master.
  r = await patchBus(SPARE, { output: { type: 'bus', target: masterId } });
  ok('a bus may not route back into a master that sends to it',
     r.status === 409, `HTTP ${r.status} ${refusal(r)}`);
  await setSends(masterId, []);

  // =========================================================================
  // 6. Cycles, counting BOTH edge kinds.
  // =========================================================================
  // The old cycle check walked the output chain only, which was exact while
  // that was the only way a bus passed audio on. These two cases run through
  // a send and out of an output, so a check that still walked one kind would
  // let them through and the loop would only be caught at topology build —
  // silently, by dropping somebody else's edge.
  await setSends(SRC, [{ id: POST, levelDb: 0, tap: 'post' }]);
  r = await patchBus(POST, { output: { type: 'bus', target: SRC } });
  ok('an OUTPUT that closes a loop through a SEND is refused',
     r.status === 409, `HTTP ${r.status} ${refusal(r)}`);

  await setSends(SRC, []);
  await patchBus(POST, { output: { type: 'bus', target: SRC } });
  r = await setSends(SRC, [{ id: POST, levelDb: 0, tap: 'post' }]);
  ok('a SEND that closes a loop through an OUTPUT is refused',
     r.status === 409, `HTTP ${r.status} ${refusal(r)}`);
  ok('the send-cycle refusal says it is about a send',
     /send/i.test(refusal(r)), refusal(r));
  await patchBus(POST, { output: { type: 'bus', target: masterId } });

  // Two buses sending to each other is a cycle with no output edge in it at
  // all — the case that only exists because sends exist.
  await setSends(POST, [{ id: SRC, levelDb: 0, tap: 'post' }]);
  r = await setSends(SRC, [{ id: POST, levelDb: 0, tap: 'post' }]);
  ok('a send-only loop between two buses is refused',
     r.status === 409, `HTTP ${r.status} ${refusal(r)}`);
  await setSends(POST, []);

  // =========================================================================
  // 7. Sends survive a round trip, and a deleted destination takes its send.
  // =========================================================================
  await setSends(SRC, [{ id: POST, levelDb: -9, tap: 'pre' }]);
  const stored = (await bus(SRC)).sends;
  ok('a send round-trips through the API intact',
     Array.isArray(stored) && stored.length === 1 && stored[0].id === POST &&
     Math.abs(stored[0].levelDb + 9) < 0.01 && stored[0].tap === 'pre',
     JSON.stringify(stored));

  // D9 retargets a lost OUTPUT to the master bus so the bus is not orphaned.
  // A send is deliberately NOT retargeted: moving a foldback onto the house
  // is the accident, not the recovery.
  await rest(`/api/buses/${POST}`, { method: 'DELETE' });
  const after = (await bus(SRC)).sends;
  ok('deleting a bus DROPS sends to it rather than retargeting them',
     Array.isArray(after) && after.length === 0, JSON.stringify(after));

  ws.close();
  console.log(failures ? `\n${failures} FAILURE(S)` : '\nALL PASS (0 failures)');
  process.exit(failures ? 1 : 0);
})().catch(e => { console.error(e); process.exit(1); });

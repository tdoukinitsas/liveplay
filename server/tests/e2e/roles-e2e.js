// R2: bus ROLES end-to-end (D24–D27, D32, D36), against a running server.
//
// Master and Preview are ordinary buses carrying a flag. The claims here are
// about what MOVING a role does to the audio, so they are measured on the
// meters rather than read back from JSON:
//   A. The output model: GET /api/outputs lists the built-ins; the master bus
//      on unmapped "Main Out" is bound and holds the house pair (0/1); a bus
//      targeting a present device name is bound, one targeting an absent name
//      is not; every hardware-bound bus reports the master pair it occupies.
//   B. The 409 matrix: role holders cannot be deleted; a role cannot be
//      dropped, only moved; one bus cannot hold both; a bus fed by another
//      cannot take the preview role; a bus-kind bus cannot take a role unless
//      the same PATCH gives it an output; nothing may feed the preview bus;
//      the master and preview buses must send to an output; the retired
//      output type "master" is still accepted and means bus->master.
//   C. Moving the PREVIEW role moves the PFL tap and the reserved pair with
//      it: the new holder carries the PFL'd bus on 30/31, the old holder
//      comes off the pair and falls silent, an active pre-listen is stopped,
//      and the house never moves (masters 0/1 unchanged, nothing loud on any
//      other pair).                                          [safety (a)]
//   D. Moving the MASTER role re-wires masters 0/1: items with no busId fall
//      back to the new holder, the house pair carries it, muting it silences
//      the house, the old holder sits on a pool pair.        [safety (b)]
//
// Both safety assertions were confirmed to FAIL against a deliberately broken
// build before being trusted (§2-6; see the R2 run log).
//
// usage: node roles-e2e.js <port> <wavPath>
const WebSocket = require(require.resolve('ws', { paths: [process.cwd()] }));

const PORT = process.argv[2] || '4480';
const WAV  = process.argv[3];
const BASE = `http://127.0.0.1:${PORT}`;

let failures = 0;
const ok = (name, pass, detail) => {
  if (detail === undefined) detail = '(no detail given — fix the assertion)';
  console.log(`${pass ? 'PASS' : 'FAIL'}  ${name}   ${detail}`);
  if (!pass) failures++;
};
const dB = x => (typeof x === 'number' ? x.toFixed(1) : String(x));
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
const buses   = async () => (await rest('/api/buses')).body;
const bus     = async id => (await buses()).find(b => b.id === id);
const master  = async () => (await buses()).find(b => b.master);
const preview = async () => (await buses()).find(b => b.preview);
const patch   = (id, body) => rest(`/api/buses/${id}`, { method: 'PATCH', body: JSON.stringify(body) });
const mk      = async (name, extra = {}) =>
  (await rest('/api/buses', { method: 'POST', body: JSON.stringify({ name, width: 2, ...extra }) })).body.id;
const play    = uuid => rest(`/api/project/items/${uuid}/play`, { method: 'POST', body: '{}' });
const stop    = uuid => rest(`/api/project/items/${uuid}/stop`, { method: 'POST', body: '{}' });
const roles   = bs => JSON.stringify(bs.map(b => [b.id, b.master ? 'M' : '', b.preview ? 'P' : '', b.masters]));

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
  masterPeak(pred) {
    let best = -200;
    for (const f of this.frames)
      for (const c of (f.master_channels || []))
        if (pred(c.index)) best = Math.max(best, c.peak_db ?? -200);
    return best;
  }
  // The house is masters 0/1 ONLY; the reserved pair is 30/31 at the default
  // width. Everything between is the pool.
  house()    { return this.masterPeak(i => i <= 1); }
  reserved() { return this.masterPeak(i => i >= 30); }
  pool()     { return this.masterPeak(i => i > 1 && i < 30); }
  active() {
    const m = new Map();
    for (const f of this.frames)
      for (const c of (f.master_channels || []))
        m.set(c.index, Math.max(m.get(c.index) ?? -200, c.peak_db ?? -200));
    return [...m].filter(([, db]) => db > -80).sort((a, b) => a[0] - b[0])
                 .map(([i, db]) => `${i}@${db.toFixed(1)}`);
  }
}
async function measure(m, ms) { m.reset(); await sleep(ms); }
async function waitLoaded() {
  for (let i = 0; i < 80; i++) {
    const p = await rest('/api/project/progress');
    if (p.body && p.body.loading === false) return;
    await sleep(100);
  }
}
// Wait until the named strip has actually fallen silent (~4 dB/s fall-back),
// so a baseline is established rather than assumed.
async function waitSilent(m, mixerId, floor = -60, maxMs = 20000) {
  let v = -200;
  for (let t = 0; t < maxMs; t += 500) {
    await measure(m, 500);
    v = m.peak(mixerId);
    if (v < floor) break;
  }
  return v;
}

(async () => {
  const ws = new WebSocket(`ws://127.0.0.1:${PORT}/ws`);
  await new Promise((res, rej) => { ws.on('open', res); ws.on('error', rej); });
  const m = new Meters(ws);

  // Fresh document with no buses key: the D35 defaults, nothing carried over.
  await rest('/api/project/document', {
    method: 'PUT',
    body: JSON.stringify({
      name: 'roles-e2e',
      items: [
        { uuid: 'item-x', type: 'audio', displayName: 'OnBus',  mediaServerPath: WAV, volume: 1, endBehavior: 'loop' },
        { uuid: 'item-y', type: 'audio', displayName: 'NoBus',  mediaServerPath: WAV, volume: 1, endBehavior: 'loop' },
      ],
    }),
  });
  await waitLoaded();
  await rest('/api/buses/pfl/clear', { method: 'POST', body: '{}' });
  await rest('/api/preview', { method: 'DELETE' });
  await rest('/api/outputs', { method: 'PUT', body: JSON.stringify({ version: 1, outputs: [] }) });

  const devs = (await rest('/api/devices')).body;
  const devName = Array.isArray(devs) && devs.length ? (devs[0].display_name || devs[0].name) : null;
  ok('a playback device is enumerated', !!devName, String(devName));

  // =========================================================================
  // A. The output model (D26/D27/D32)
  // =========================================================================
  const outs = (await rest('/api/outputs')).body;
  ok('A: GET /api/outputs lists the built-in names',
     Array.isArray(outs.builtin) && outs.builtin.includes('Main Out') && outs.builtin.includes('Preview Out'),
     JSON.stringify(outs.builtin));
  let M = await master(), P = await preview();
  ok('A: the master bus on unmapped Main Out is bound and holds the house pair',
     M.output.target === 'Main Out' && M.bound === true && JSON.stringify(M.masters) === '[0,1]',
     `${JSON.stringify(M.output)} bound ${M.bound} masters ${JSON.stringify(M.masters)}`);
  ok('A: the preview bus on unmapped Preview Out is unbound',
     P.output.target === 'Preview Out' && P.bound === false,
     `${JSON.stringify(P.output)} bound ${P.bound}`);

  const Dev   = await mk('Dev',   { output: { type: 'output', target: devName } });
  const Ghost = await mk('Ghost', { output: { type: 'output', target: 'No Such Device (roles-e2e)' } });
  let bDev = await bus(Dev), bGhost = await bus(Ghost);
  ok('A: a bus targeting a present device name reports bound:true',
     bDev.bound === true, `"${bDev.output.target}" bound ${bDev.bound}`);
  ok('A: ...and reports the pool pair it occupies',
     Array.isArray(bDev.masters) && bDev.masters[0] >= 2 && bDev.masters[1] === bDev.masters[0] + 1,
     `masters ${JSON.stringify(bDev.masters)}`);
  ok('A: a bus targeting an absent device name reports bound:false',
     bGhost.bound === false, `"${bGhost.output.target}" bound ${bGhost.bound}`);
  let all = await buses();
  ok('A: every hardware-bound Output-kind bus reports its master pair',
     all.filter(b => b.bound && b.output.type === 'output').every(b => Array.isArray(b.masters) && b.masters.length === 2),
     roles(all));
  ok('A: a bus->bus send reports masters:null',
     (await (async () => { const id = await mk('Send'); const b = await bus(id); await rest(`/api/buses/${id}`, { method: 'DELETE' }); return b; })()).masters === null,
     '(a fresh POST /api/buses defaults to a send into the master bus)');
  await rest(`/api/buses/${Ghost}`, { method: 'DELETE' });

  // =========================================================================
  // B. The 409 matrix (D24/D25)
  // =========================================================================
  const is409 = (r, text) => r.status === 409 && r.body && r.body.error === text;
  const show  = r => `status ${r.status} ${JSON.stringify(r.body)}`;
  let r;
  r = await rest(`/api/buses/${M.id}`, { method: 'DELETE' });
  ok('B: DELETE of the master holder is 409 with the exact text',
     is409(r, 'this bus holds the Master role; move it first'), show(r));
  r = await rest(`/api/buses/${P.id}`, { method: 'DELETE' });
  ok('B: DELETE of the preview holder is 409 with the exact text',
     is409(r, 'this bus holds the Preview role; move it first'), show(r));
  ok('B: ...and both are still there', !!(await bus(M.id)) && !!(await bus(P.id)), roles(await buses()));
  r = await patch(P.id, { preview: false });
  ok('B: {preview:false} is 409 — a role is moved, never dropped',
     is409(r, 'move the role to another bus instead'), show(r));
  r = await patch(M.id, { master: false });
  ok('B: {master:false} is 409 with the same text',
     is409(r, 'move the role to another bus instead'), show(r));
  r = await patch(P.id, { master: true });
  ok('B: {master:true} on the preview holder is 409 RoleConflict',
     is409(r, 'one bus cannot hold both the Master and Preview roles'), show(r));
  r = await patch(M.id, { preview: true });
  ok('B: {preview:true} on the master holder is 409 RoleConflict',
     is409(r, 'one bus cannot hold both the Master and Preview roles'), show(r));

  // A bus that is fed by another cannot become the preview bus (the output is
  // given in the same PATCH so that RoleNeedsOutput cannot be the reason).
  const Fed    = await mk('Fed');
  const Feeder = await mk('Feeder', { output: { type: 'bus', target: Fed } });
  ok('B: Feeder -> Fed was accepted', (await bus(Feeder)).output.target === Fed, JSON.stringify((await bus(Feeder)).output));
  r = await patch(Fed, { preview: true, output: { type: 'output', target: 'Preview Out' } });
  ok('B: {preview:true} on a bus fed by another is 409 RoleTargetFed',
     is409(r, 'buses feed this bus; re-route them before making it the Preview bus'), show(r));
  ok('B: ...and the refused PATCH changed nothing', (await bus(Fed)).output.type === 'bus' && !(await bus(Fed)).preview,
     JSON.stringify((await bus(Fed)).output));

  // A bus-kind bus cannot take a role; the same PATCH with an output can.
  const K = await mk('K');
  r = await patch(K, { preview: true });
  ok('B: {preview:true} on a bus-kind bus is 409 RoleNeedsOutput',
     is409(r, 'a bus must send to an output to hold the Master or Preview role'), show(r));
  r = await patch(K, { master: true });
  ok('B: {master:true} on a bus-kind bus is 409 RoleNeedsOutput',
     is409(r, 'a bus must send to an output to hold the Master or Preview role'), show(r));
  r = await patch(K, { preview: true, output: { type: 'output', target: 'Preview Out' } });
  ok('B: the same PATCH with output:{output,"Preview Out"} succeeds', r.status === 200, show(r));
  all = await buses();
  ok('B: K now holds the preview role and the old holder does not — exactly one of each',
     all.find(b => b.id === K).preview === true && all.find(b => b.id === P.id).preview === false &&
     all.filter(b => b.preview).length === 1 && all.filter(b => b.master).length === 1,
     roles(all));
  r = await patch(P.id, { preview: true });
  ok('B: the role moves back to the stock preview bus', r.status === 200 && (await preview()).id === P.id, show(r));

  // Routing rules.
  r = await patch(Feeder, { output: { type: 'bus', target: P.id } });
  ok('B: any bus -> the preview bus is 409 IllegalTarget',
     is409(r, 'a bus cannot feed the Preview bus'), show(r));
  r = await patch(M.id, { output: { type: 'bus', target: Fed } });
  ok('B: the master bus -> a bus is 409',
     is409(r, 'the Master bus must send to an output'), show(r));
  r = await patch(P.id, { output: { type: 'bus', target: Fed } });
  ok('B: the preview bus -> a bus is 409',
     is409(r, 'the Preview bus cannot be routed to another bus'), show(r));
  r = await patch(P.id, { output: { type: 'master', target: '' } });
  ok('B: ...and so is the retired type "master" on the preview bus (it means bus->master)',
     is409(r, 'the Preview bus cannot be routed to another bus'), show(r));
  // The retired spelling still works for a controller written against round 1.
  const Legacy = await mk('Legacy', { output: { type: 'master', target: '' } });
  ok('B: POST with output.type "master" is accepted and mapped to {bus, <master id>}',
     (await bus(Legacy)).output.type === 'bus' && (await bus(Legacy)).output.target === M.id,
     JSON.stringify((await bus(Legacy)).output));
  r = await patch(Legacy, { output: { type: 'output', target: 'Main Out' } });
  r = await patch(Legacy, { output: { type: 'master', target: '' } });
  ok('B: PATCH with output.type "master" is accepted and mapped too',
     r.status === 200 && (await bus(Legacy)).output.type === 'bus' && (await bus(Legacy)).output.target === M.id,
     `${show(r)} -> ${JSON.stringify((await bus(Legacy)).output)}`);
  // A reorder renumbers the rail densely (D31); the role holders keep theirs.
  const orderBefore = (await buses()).filter(b => !b.master && !b.preview).map(b => `${b.id}:${b.order}`);
  r = await patch(Legacy, { order: 1 });
  const rail = (await buses()).filter(b => !b.master && !b.preview).sort((a, b) => a.order - b.order);
  ok('B: PATCH {order} renumbers the rail 1..N with the moved bus where asked',
     r.status === 200 && rail[0].id === Legacy && rail.every((b, i) => b.order === i + 1),
     `${orderBefore.join(' ')} -> ${rail.map(b => `${b.id}:${b.order}`).join(' ')}`);
  ok('B: ...and the role holders keep their own sort keys',
     (await master()).order === M.order && (await preview()).order === P.order,
     `master ${(await master()).order} preview ${(await preview()).order}`);
  for (const id of [Feeder, Fed, K, Legacy]) await rest(`/api/buses/${id}`, { method: 'DELETE' });

  // =========================================================================
  // C. Moving the PREVIEW role moves the PFL tap and the reserved pair
  // =========================================================================
  // Preview Out is mapped to a device so the reserved pair is actually driven
  // and can be metered; the map is put back at the end.
  if (!devName) { console.log('SKIP  C and D need a playback device'); }
  else {
    await rest('/api/outputs', {
      method: 'PUT',
      body: JSON.stringify({ version: 1, outputs: [
        { name: 'Preview Out', channels: [{ device: devName, hwChannel: 0 }, { device: devName, hwChannel: 1 }] },
      ]}),
    });
    const T  = await mk('T');                                                  // carries item-x
    const P2 = await mk('P2', { output: { type: 'output', target: 'Preview Out' } }); // the next holder
    await rest('/api/project/items/item-x', { method: 'PATCH', body: JSON.stringify({ busId: T }) });
    const mT = (await bus(T)).mixerId, mP = P.mixerId, mP2 = (await bus(P2)).mixerId;
    ok('C: strips exist for T, the preview bus and P2', !!mT && !!mP && !!mP2, `${mT} ${mP} ${mP2}`);

    await play('item-x');
    await sleep(500);
    const pQuiet = await waitSilent(m, mP);
    ok('C: the preview strip is silent before PFL', pQuiet < -60, `${dB(pQuiet)} dBFS`);
    await measure(m, 800);
    const houseBase = m.house();
    ok('C: the house carries T (item-x) through the master bus', houseBase > -30, `${dB(houseBase)} dBFS`);

    await rest(`/api/buses/${T}/pfl`, { method: 'POST', body: JSON.stringify({ pfl: true }) });
    await rest('/api/preview', { method: 'POST', body: JSON.stringify({ itemUuid: 'item-y' }) });
    await sleep(800);
    await measure(m, 800);
    const pBefore = m.peak(mP), resBefore = m.reserved(), p2Before = m.peak(mP2);
    ok('C: PFL reaches the preview strip', pBefore > -30, `${dB(pBefore)} dBFS`);
    ok('C: ...and the reserved pair carries it', resBefore > -30, `${dB(resBefore)} dBFS  masters ${m.active().join(' ')}`);
    ok('C: P2 hears nothing yet', p2Before < -60, `${dB(p2Before)} dBFS`);
    ok('C: a pre-listen is active', (await rest('/api/preview')).body.active === true,
       JSON.stringify((await rest('/api/preview')).body));
    ok('C: the preview holder reports the reserved pair', JSON.stringify((await preview()).masters) === '[30,31]',
       JSON.stringify((await preview()).masters));

    // ---- the move ----
    r = await patch(P2, { preview: true });
    ok('C: PATCH P2 {preview:true} is accepted', r.status === 200, show(r));
    await sleep(1500);
    all = await buses();
    const p2After = all.find(b => b.id === P2), pOld = all.find(b => b.id === P.id), tAfter = all.find(b => b.id === T);
    ok('C: P2 holds the preview role; the old holder does not',
       p2After.preview === true && pOld.preview === false && all.filter(b => b.preview).length === 1, roles(all));
    ok('C: P2 reports the reserved pair', JSON.stringify(p2After.masters) === '[30,31]', JSON.stringify(p2After.masters));
    ok('C: the old holder is off the reserved pair (null or a pool pair)',
       pOld.masters === null || (pOld.masters[0] >= 2 && pOld.masters[0] < 30), JSON.stringify(pOld.masters));
    ok('C: the house pair did not move (master still on 0/1)',
       JSON.stringify(all.find(b => b.master).masters) === '[0,1]', roles(all));
    ok('C: the pre-listen was stopped by the move', (await rest('/api/preview')).body.active === false,
       JSON.stringify((await rest('/api/preview')).body));
    ok('C: T still reports PFL', tAfter.pfl === true, `pfl ${tAfter.pfl}`);
    ok('C: the new holder reports PFL cleared and mono-check off', p2After.pfl === false && p2After.monoCheck === false,
       `pfl ${p2After.pfl} monoCheck ${p2After.monoCheck}`);

    await measure(m, 900);
    const p2Now = m.peak(mP2), pNow = m.peak(mP), resNow = m.reserved(), houseNow = m.house();
    ok('C: the new holder carries the PFL tap', p2Now > -30, `P2 ${dB(p2Now)} dBFS`);
    ok('C: the reserved pair still carries the tap (now from P2)', resNow > -30, `${dB(resNow)} dBFS  masters ${m.active().join(' ')}`);
    ok('C: the old holder has fallen silent', pNow < pBefore - 12, `${dB(pBefore)} -> ${dB(pNow)} dBFS`);
    // [safety (a)] preview never reaches the house after a role move: the
    // house level is what it was, and no pair other than 0/1 and 30/31 is
    // carrying anything near it (a decaying tail is tens of dB down).
    const loudOther = m.active().map(s => s.split('@')).filter(([i, db]) => Number(i) > 1 && Number(i) < 30 && Number(db) > houseBase - 20);
    ok('C: [safety] the house level is unchanged by the preview-role move',
       Math.abs(houseNow - houseBase) < 1.0, `${dB(houseBase)} -> ${dB(houseNow)} dBFS`);
    ok('C: [safety] nothing PFL\'d reaches any pair but the reserved one',
       loudOther.length === 0, `masters ${m.active().join(' ')}`);

    // And back: the stock bus takes the role again, P2 comes off the pair.
    r = await patch(P.id, { preview: true });
    await sleep(1500);
    await measure(m, 900);
    all = await buses();
    ok('C: moving the role back puts the stock preview bus on the reserved pair with the tap',
       r.status === 200 && JSON.stringify(all.find(b => b.id === P.id).masters) === '[30,31]' && m.peak(mP) > -30 &&
       (all.find(b => b.id === P2).masters === null || all.find(b => b.id === P2).masters[0] < 30),
       `${roles(all)} P ${dB(m.peak(mP))} dBFS`);
    ok('C: ...and P2 falls silent', m.peak(mP2) < p2Now - 12, `${dB(p2Now)} -> ${dB(m.peak(mP2))} dBFS`);
    await rest('/api/buses/pfl/clear', { method: 'POST', body: '{}' });
    await rest('/api/preview', { method: 'DELETE' });
    await stop('item-x');
    await rest(`/api/buses/${P2}`, { method: 'DELETE' });
    await rest('/api/outputs', { method: 'PUT', body: JSON.stringify({ version: 1, outputs: [] }) });

    // =======================================================================
    // D. Moving the MASTER role re-wires the house pair
    // =======================================================================
    await sleep(1500);
    const Y = await mk('Y', { output: { type: 'output', target: 'Main Out' } });
    const mY = (await bus(Y)).mixerId, mM = M.mixerId;
    const yBefore = await bus(Y);
    ok('D: Y (Output-kind, Main Out) is bound on a pool pair before the move',
       yBefore.bound === true && Array.isArray(yBefore.masters) && yBefore.masters[0] >= 2 && yBefore.masters[0] < 30,
       `masters ${JSON.stringify(yBefore.masters)}`);
    // item-y has no busId: it inherits the master bus.
    await play('item-y');
    await sleep(600);
    await measure(m, 800);
    const houseD = m.house(), mStripD = m.peak(mM);
    ok('D: item-y (no busId) plays through the master bus', mStripD > -30, `${dB(mStripD)} dBFS`);
    ok('D: ...and out the house', houseD > -30, `${dB(houseD)} dBFS`);
    ok('D: the master bus lists item-y', (await master()).itemUuids.includes('item-y'), JSON.stringify((await master()).itemUuids));

    r = await patch(Y, { master: true });
    ok('D: PATCH Y {master:true} is accepted', r.status === 200, show(r));
    await sleep(1500);
    all = await buses();
    const yAfter = all.find(b => b.id === Y), mOld = all.find(b => b.id === M.id);
    ok('D: Y holds the master role; the old holder does not; exactly one master',
       yAfter.master === true && mOld.master === false && all.filter(b => b.master).length === 1, roles(all));
    ok('D: [safety] Y is on the house pair 0/1', JSON.stringify(yAfter.masters) === '[0,1]', JSON.stringify(yAfter.masters));
    ok('D: the old master sits on a pool pair',
       Array.isArray(mOld.masters) && mOld.masters[0] >= 2 && mOld.masters[0] < 30, JSON.stringify(mOld.masters));
    ok('D: item-y now falls back to Y (itemUuids moved)',
       yAfter.itemUuids.includes('item-y') && !mOld.itemUuids.includes('item-y'),
       `Y ${JSON.stringify(yAfter.itemUuids)} old ${JSON.stringify(mOld.itemUuids)}`);
    await measure(m, 900);
    const yStrip = m.peak(mY), mStripAfter = m.peak(mM), houseAfter = m.house();
    ok('D: Y\'s strip carries item-y', yStrip > -30, `${dB(yStrip)} dBFS`);
    ok('D: the old master\'s strip gave it up', mStripAfter < mStripD - 12, `${dB(mStripD)} -> ${dB(mStripAfter)} dBFS`);
    ok('D: [safety] the house pair carries Y at the level it carried the old master',
       houseAfter > -30 && Math.abs(houseAfter - houseD) < 1.5, `${dB(houseD)} -> ${dB(houseAfter)} dBFS`);
    // Mute the new holder: the house goes quiet. That is the proof the house
    // IS Y now, not that Y merely also reaches the same device.
    await patch(Y, { mute: true });
    await sleep(1500);
    await measure(m, 800);
    const houseMuted = m.house();
    ok('D: [safety] muting Y silences the house (which was carrying it)',
       houseAfter > -30 && houseMuted < houseAfter - 12, `${dB(houseAfter)} -> ${dB(houseMuted)} dBFS`);
    await patch(Y, { mute: false });
    await sleep(500);

    // Back again, so the next script finds the stock layout.
    r = await patch(M.id, { master: true });
    await sleep(1500);
    all = await buses();
    ok('D: the role moves back to the stock master bus, on 0/1, with item-y',
       r.status === 200 && all.find(b => b.id === M.id).master && JSON.stringify(all.find(b => b.id === M.id).masters) === '[0,1]' &&
       all.find(b => b.id === M.id).itemUuids.includes('item-y'), roles(all));
    await measure(m, 800);
    ok('D: ...and the house follows it back', m.house() > -30 && m.peak(mM) > -30, `house ${dB(m.house())} strip ${dB(m.peak(mM))} dBFS`);
    await stop('item-y');
    await rest(`/api/buses/${Y}`, { method: 'DELETE' });
  }

  await rest(`/api/buses/${Dev}`, { method: 'DELETE' });
  await rest('/api/outputs', { method: 'PUT', body: JSON.stringify({ version: 1, outputs: [] }) });
  ws.close();
  console.log(`\n${failures === 0 ? 'ALL PASS' : 'FAILURES'} (${failures})`);
  process.exit(failures === 0 ? 0 : 1);
})().catch(e => { console.error('harness error:', e); process.exit(2); });

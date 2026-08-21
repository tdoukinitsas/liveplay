// A3: bus->bus e2e. Real audio, real render loop, against a running server.
//
// A1 built the ordered-strip render pass (chain -> PFL tap -> fader ->
// bus->bus send -> destination) and A2 wired the REST/validation layer on
// top of it (D6 cycle refusal, D9 delete-retarget, D8 lane-gain reuse). This
// asserts the six behavioural claims in the completion plan against that
// build, using two signals:
//   WAV  - mono/identical-lanes: L and R carry the SAME samples, in phase.
//          Any gain law applied to it is exact arithmetic, not phase luck,
//          so it is used everywhere a precise number is asserted.
//   WIDE - has side content: L and R differ. It is used only where the
//          claim is specifically about a stereo->mono FOLD (assertion 3b),
//          so a bug that just forwards one lane unchanged cannot pass by
//          coincidence the way it could on identical-lane content.
//
// usage: node busbus-e2e.js <port> <wavPath> [widePath]
// widePath defaults to wavPath with "sig.wav" replaced by "wide.wav".
const WebSocket = require(require.resolve('ws', { paths: [process.cwd()] }));

const PORT = process.argv[2] || '4580';
const WAV  = process.argv[3];
const WIDE = process.argv[4] || WAV.replace(/sig\.wav$/i, 'wide.wav');
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

// dB <-> linear, for combining measured lane peaks into a predicted fold.
const lin = db => Math.pow(10, db / 20);
const db  = x  => 20 * Math.log10(Math.max(x, 1e-9));

class Meters {
  constructor(ws) {
    this.frames = [];
    ws.on('message', raw => {
      let m; try { m = JSON.parse(raw); } catch { return; }
      if (m.type === 'meters') this.frames.push(m);
    });
  }
  reset() { this.frames = []; }
  // Highest peak_db seen for one mixer strip over the collection window.
  peak(mixerId) {
    let best = -200;
    for (const f of this.frames)
      for (const c of (f.mixer_channels || []))
        if (c.mixer_id === mixerId) best = Math.max(best, c.peak_db ?? -200);
    return best;
  }
  // Per-lane peaks for one strip: [laneL, laneR], each the max over the
  // window, taken only from frames where the strip actually had signal (a
  // silent frame's stale lane values would otherwise pollute the max).
  lanes(mixerId) {
    let l = -200, r = -200, seen = false;
    for (const f of this.frames)
      for (const c of (f.mixer_channels || []))
        if (c.mixer_id === mixerId && c.lanes && c.lanes.length >= 2) {
          if (c.peak_db > -100) {
            seen = true;
            l = Math.max(l, c.lanes[0].peak_db ?? -200);
            r = Math.max(r, c.lanes[1].peak_db ?? -200);
          }
        }
    return seen ? [l, r] : [-200, -200];
  }
  // Same, but RMS. For decorrelated content, peaks of the two lanes rarely
  // coincide in time, so a fold predicted from PEAKS understates what a
  // sample-exact sum needs (a lane's own peak sample is rarely also the
  // moment the other lane peaks). RMS is the quantity that actually adds in
  // power for uncorrelated signals, which is what makes a numeric fold
  // prediction meaningful on wide.wav rather than on identical-lane content
  // only.
  rmsLanes(mixerId) {
    let l = -200, r = -200, seen = false;
    for (const f of this.frames)
      for (const c of (f.mixer_channels || []))
        if (c.mixer_id === mixerId && c.lanes && c.lanes.length >= 2) {
          if (c.peak_db > -100) {
            seen = true;
            l = Math.max(l, c.lanes[0].rms_db ?? -200);
            r = Math.max(r, c.lanes[1].rms_db ?? -200);
          }
        }
    return seen ? [l, r] : [-200, -200];
  }
  rms(mixerId) {
    let best = -200;
    for (const f of this.frames)
      for (const c of (f.mixer_channels || []))
        if (c.mixer_id === mixerId && c.peak_db > -100) best = Math.max(best, c.rms_db ?? -200);
    return best;
  }
  master(ch) {
    let best = -200;
    for (const f of this.frames)
      for (const c of (f.master_channels || []))
        if (c.index === ch) best = Math.max(best, c.peak_db ?? -200);
    return best;
  }
  house() { return Math.max(this.master(0), this.master(1)); }
  // Inter-lane correlation (+1 mono-compatible/identical, 0 wide), averaged
  // over frames where the strip had signal. Used to confirm a "wide" source
  // is actually decorrelated rather than trusting the file name.
  correlation(mixerId) {
    let sum = 0, n = 0;
    for (const f of this.frames)
      for (const c of (f.mixer_channels || []))
        if (c.mixer_id === mixerId && c.peak_db > -100 && typeof c.correlation === 'number') {
          sum += c.correlation; n++;
        }
    return n ? sum / n : NaN;
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
async function play(uuid)  { await rest(`/api/project/items/${uuid}/play`, { method: 'POST', body: '{}' }); }
async function stop(uuid)  { await rest(`/api/project/items/${uuid}/stop`, { method: 'POST', body: '{}' }); }
async function setBus(uuid, busId) {
  await rest(`/api/project/items/${uuid}`, { method: 'PATCH', body: JSON.stringify({ busId }) });
}

(async () => {
  const ws = new WebSocket(`ws://127.0.0.1:${PORT}/ws`);
  await new Promise((res, rej) => { ws.on('open', res); ws.on('error', rej); });
  const m = new Meters(ws);

  // ---- Establish a clean starting state ourselves ----
  // pfl-e2e's known quirk (fails its first assertion on a second run against
  // the same server) comes from asserting state instead of establishing it.
  // A fresh document with no buses key sidesteps that: every id below is
  // created from scratch, so nothing carries over from a previous run.
  await rest('/api/project/document', {
    method: 'PUT',
    body: JSON.stringify({
      name: 'busbus-e2e',
      items: [
        { uuid: 'item-a',    type: 'audio', displayName: 'ChainSig',  mediaServerPath: WAV,  volume: 1, endBehavior: 'loop' },
        { uuid: 'item-ctrl', type: 'audio', displayName: 'CtrlSig',   mediaServerPath: WAV,  volume: 1, endBehavior: 'loop' },
        { uuid: 'item-m',    type: 'audio', displayName: 'MonoSig',   mediaServerPath: WAV,  volume: 1, endBehavior: 'loop' },
        { uuid: 'item-w',    type: 'audio', displayName: 'WideSig',   mediaServerPath: WIDE, volume: 1, endBehavior: 'loop' },
      ],
    }),
  });
  await waitLoaded();

  // ---- Buses ----
  //   Ctrl -> master              (single-hop control for assertion 1)
  //   A -> B -> master            (the chain under test: 1, 2, 4, 5, 6)
  //   M -> S -> master            (mono bus feeding stereo: assertion 3a)
  //   W -> T -> master            (stereo bus feeding mono: assertion 3b)
  const mk = async (name, width) =>
    (await rest('/api/buses', { method: 'POST', body: JSON.stringify({ name, width }) })).body.id;
  const Ctrl = await mk('Ctrl', 2);
  const A    = await mk('A', 2);
  const B    = await mk('B', 2);
  const M    = await mk('M', 1);
  const S    = await mk('S', 2);
  const W    = await mk('W', 2);
  const T    = await mk('T', 1);

  await rest(`/api/buses/${A}`, { method: 'PATCH', body: JSON.stringify({ output: { type: 'bus', target: B } }) });
  await rest(`/api/buses/${M}`, { method: 'PATCH', body: JSON.stringify({ output: { type: 'bus', target: S } }) });
  await rest(`/api/buses/${W}`, { method: 'PATCH', body: JSON.stringify({ output: { type: 'bus', target: T } }) });

  await setBus('item-a',    A);
  await setBus('item-ctrl', Ctrl);
  await setBus('item-m',    M);
  await setBus('item-w',    W);

  const mixerIdOf = async id => (await bus(id)).mixerId;
  const [mCtrl, mA, mB, mM, mS, mW, mT] =
    await Promise.all([Ctrl, A, B, M, S, W, T].map(mixerIdOf));

  // =========================================================================
  // 1. Chain of unity gains == direct-to-master, on the master meters.
  // =========================================================================
  // Sequential, not simultaneous: isolating each path onto the master meter
  // is what makes "the expected level" checkable there at all, per the plan.
  await play('item-ctrl');
  await sleep(500);
  await measure(m, 800);
  const masterDirect = m.house();
  ok('direct-to-master (Ctrl) is audible', masterDirect > -20, `${masterDirect.toFixed(1)} dBFS`);
  await stop('item-ctrl');
  await sleep(600); // let Ctrl's peak-hold decay out of the next window's story

  await play('item-a');
  await sleep(500);
  await measure(m, 800);
  const masterChain = m.house();
  ok('A->B->master is audible on the master meters', masterChain > -20, `${masterChain.toFixed(1)} dBFS`);
  ok('...at the same level as direct-to-master (unity chain)',
     Math.abs(masterChain - masterDirect) < 1.5,
     `direct ${masterDirect.toFixed(1)} vs chain ${masterChain.toFixed(1)} dBFS`);
  // item-a stays playing through A->B for assertions 2, 4, 5, 6.

  // =========================================================================
  // 2. A's fader attenuates the chain; B's fader attenuates the chain;
  //    muting B silences it.
  // =========================================================================
  await measure(m, 800);
  const baseline2 = m.house();

  // set_gain_db() is instant on the strip (no ramp), but the METER'S own
  // peak-hold ballistics take a moment to fall from the old level to the
  // new one — see the mute test below, which already accounts for this.
  // 300 ms was not enough for the fader case either; 1200 ms is.
  await rest(`/api/buses/${A}`, { method: 'PATCH', body: JSON.stringify({ gainDb: -12 }) });
  await sleep(1200);
  await measure(m, 800);
  const afterAFader = m.house();
  ok('A\'s fader attenuates the chain by ~12 dB',
     Math.abs((baseline2 - afterAFader) - 12) < 2.0,
     `${baseline2.toFixed(1)} -> ${afterAFader.toFixed(1)} dBFS`);
  await rest(`/api/buses/${A}`, { method: 'PATCH', body: JSON.stringify({ gainDb: 0 }) });
  await sleep(1200);

  await rest(`/api/buses/${B}`, { method: 'PATCH', body: JSON.stringify({ gainDb: -12 }) });
  await sleep(1200);
  await measure(m, 800);
  const afterBFader = m.house();
  ok('B\'s fader attenuates the chain by ~12 dB',
     Math.abs((baseline2 - afterBFader) - 12) < 2.0,
     `${baseline2.toFixed(1)} -> ${afterBFader.toFixed(1)} dBFS`);
  await rest(`/api/buses/${B}`, { method: 'PATCH', body: JSON.stringify({ gainDb: 0 }) });
  await sleep(1200);

  await rest(`/api/buses/${B}`, { method: 'PATCH', body: JSON.stringify({ mute: true }) });
  await sleep(1500); // let the master peak-hold decay before asserting silence
  await measure(m, 800);
  const mutedHouse = m.house();
  ok('muting B silences the chain (below -30 dBFS, allowing for peak-hold decay)',
     mutedHouse < -30, `${mutedHouse.toFixed(1)} dBFS`);
  await rest(`/api/buses/${B}`, { method: 'PATCH', body: JSON.stringify({ mute: false }) });
  await sleep(500);
  await measure(m, 800);
  ok('unmuting B restores the chain', m.house() > -20, `${m.house().toFixed(1)} dBFS`);

  // =========================================================================
  // 3. Pan/downmix laws on bus->bus edges (D8: same laws as mixer->master).
  // =========================================================================
  // item-a is paused for this section: three -6 dBFS signals summing onto
  // the master at once can trip the brick-wall limiter, whose release time
  // then contaminates the NEXT section's measurements long after the
  // transient is gone. M/S and W/T are their own strips, so nothing here
  // needs the chain playing to be checked.
  await stop('item-a');
  await sleep(300);
  await play('item-m');
  await play('item-w');
  await sleep(600);

  // 3a. Mono bus (M) feeding a stereo bus (S): pan law, centred -> both lanes
  //     at kDefaultDownmixDb below M's own level. Deterministic: it's a pure
  //     gain application on one signal, no phase ambiguity.
  await measure(m, 800);
  const mLevel = m.peak(mM);
  const [sL, sR] = m.lanes(mS);
  ok('M (mono bus) is audible', mLevel > -20, `${mLevel.toFixed(1)} dBFS`);
  ok('S\'s lanes are symmetric (centred pan)', Math.abs(sL - sR) < 1.0,
     `L ${sL.toFixed(1)} / R ${sR.toFixed(1)} dBFS`);
  const expectS = mLevel - 3.0; // kDefaultDownmixDb, matching pan_gains_db(0).left
  ok('S\'s lanes land at M\'s level - 3 dB (pan law centre == kDefaultDownmixDb)',
     Math.abs(sL - expectS) < 1.5 && Math.abs(sR - expectS) < 1.5,
     `M ${mLevel.toFixed(1)} -> S ${sL.toFixed(1)}/${sR.toFixed(1)} dBFS (expected ~${expectS.toFixed(1)})`);

  // 3b. Stereo bus (W, decorrelated content) feeding a mono bus (T): fold at
  //     kDefaultDownmixDb per lane, summed. Predicted from W's OWN measured
  //     lane peaks (no assumption about how W got its level), so this is a
  //     direct check of the D8 claim rather than of anything upstream of it.
  const [wL, wR] = m.lanes(mW);
  const tLevel = m.peak(mT);
  const wCorr  = m.correlation(mW);
  // Peak amplitude alone does not prove decorrelation (two different
  // waveforms can happen to share a peak) — inter-lane correlation does:
  // +1 is identical/mono-compatible, and "wide" content sits well below it.
  ok('W\'s two lanes are genuinely decorrelated (it is really stereo, not just named wide)',
     wCorr < 0.9, `correlation ${wCorr.toFixed(3)} (L ${wL.toFixed(1)} / R ${wR.toFixed(1)} dBFS)`);
  ok('T (the mono fold) is audible', tLevel > -20, `${tLevel.toFixed(1)} dBFS`);
  // Predicted in the RMS domain (power adds for near-decorrelated signals;
  // correlation was just confirmed ~0), not the peak domain (peaks of two
  // decorrelated lanes rarely coincide, so a peak-domain sum systematically
  // overstates the fold and was the wrong tool for this signal — see the
  // failed attempt this replaced, kept in the report rather than the code).
  const [wRmsL, wRmsR] = m.rmsLanes(mW);
  const tRms = m.rms(mT);
  const g = lin(-3.0);
  const predictedTRms = db(Math.sqrt((lin(wRmsL) * g) ** 2 + (lin(wRmsR) * g) ** 2));
  ok('T\'s RMS lands near the kDefaultDownmixDb 2->1 fold predicted from W\'s own lane RMS',
     Math.abs(tRms - predictedTRms) < 2.0,
     `W rms ${wRmsL.toFixed(1)}/${wRmsR.toFixed(1)} -> predicted ${predictedTRms.toFixed(1)}, measured T ${tRms.toFixed(1)} dBFS`);
  // And it is not simply one lane forwarded unchanged, which would coincide
  // with a correct-looking number on identical-lane content but not on this.
  ok('T is not just one of W\'s lanes passed through unfolded',
     Math.abs(tLevel - wL) > 0.8 || Math.abs(tLevel - wR) > 0.8,
     `T ${tLevel.toFixed(1)} vs W.L ${wL.toFixed(1)} / W.R ${wR.toFixed(1)} dBFS`);

  await stop('item-m');
  await stop('item-w');
  await sleep(1500); // let any limiter release settle before resuming the chain
  await play('item-a');
  await sleep(500);
  await measure(m, 800);
  ok('the chain is back to its baseline level before assertion 4',
     Math.abs(m.house() - baseline2) < 2.0, `${m.house().toFixed(1)} dBFS (baseline ${baseline2.toFixed(1)})`);

  // =========================================================================
  // 4. A cycle PATCH is refused (409) and the audio is undisturbed: the seam
  //    counter in GET /api/engine/stats is unchanged across it.
  // =========================================================================
  await sleep(500);
  const statsBefore = (await rest('/api/engine/stats')).body;
  const cyc = await rest(`/api/buses/${B}`, { method: 'PATCH', body: JSON.stringify({ output: { type: 'bus', target: A } }) });
  ok('B -> A is refused with 409', cyc.status === 409, `status ${cyc.status}`);
  ok('...with the exact D6 cycle message',
     cyc.body && cyc.body.error === 'routing this bus would create a cycle',
     JSON.stringify(cyc.body));
  await sleep(500);
  const statsAfter = (await rest('/api/engine/stats')).body;
  ok('the seam counter is unchanged across the refused cycle patch',
     statsAfter.discontinuities === statsBefore.discontinuities,
     `discontinuities ${statsBefore.discontinuities} -> ${statsAfter.discontinuities}`);
  await measure(m, 800);
  ok('and the chain is still audible, undisturbed', m.house() > -20, `${m.house().toFixed(1)} dBFS`);
  const bAfterCycle = await bus(B);
  ok('B still points at master, not A (the refused patch changed nothing)',
     bAfterCycle.output.type === 'master', JSON.stringify(bAfterCycle.output));

  // =========================================================================
  // 5. Deleting the middle bus (B) retargets its feeder (A) to master, and
  //    audio continues without a gap.
  // =========================================================================
  const del = await rest(`/api/buses/${B}`, { method: 'DELETE' });
  ok('DELETE B is 200', del.status === 200, `status ${del.status}`);
  const aAfterDelete = await bus(A);
  ok('A was retargeted to the master (D9)',
     aAfterDelete.output.type === 'master', JSON.stringify(aAfterDelete.output));
  await sleep(300);
  await measure(m, 800);
  ok('audio continues after the retarget (no silence)', m.house() > -20, `${m.house().toFixed(1)} dBFS`);

  // =========================================================================
  // 6. PFL on A still taps pre-fader/pre-mute; the house is unaffected.
  // =========================================================================
  await rest(`/api/buses/${A}/pfl`, { method: 'POST', body: JSON.stringify({ pfl: true }) });
  await sleep(300);
  await measure(m, 800);
  const houseBeforeFaderDown = m.house();
  const monitorId = (await bus('monitor')).mixerId;
  const monitorBeforeFaderDown = m.peak(monitorId);
  ok('PFL is audible in Monitor before touching the fader',
     monitorBeforeFaderDown > -20, `${monitorBeforeFaderDown.toFixed(1)} dBFS`);

  await rest(`/api/buses/${A}`, { method: 'PATCH', body: JSON.stringify({ gainDb: -60, mute: true }) });
  await sleep(1500); // peak-hold decay in the house before asserting silence
  await measure(m, 800);
  const houseAfterFaderDown = m.house();
  const monitorAfterFaderDown = m.peak(monitorId);
  ok('the house goes quiet with A\'s fader down and muted',
     houseAfterFaderDown < -30, `${houseAfterFaderDown.toFixed(1)} dBFS`);
  ok('...but Monitor still hears A: PFL is pre-fader and pre-mute',
     monitorAfterFaderDown > -20, `${monitorAfterFaderDown.toFixed(1)} dBFS`);
  ok('and PFL never changed what the house heard beforehand',
     houseBeforeFaderDown > -20, `${houseBeforeFaderDown.toFixed(1)} dBFS`);

  await rest(`/api/buses/${A}`, { method: 'PATCH', body: JSON.stringify({ gainDb: 0, mute: false }) });
  await rest(`/api/buses/${A}/pfl`, { method: 'POST', body: JSON.stringify({ pfl: false }) });
  await stop('item-a');

  ws.close();
  console.log(`\n${failures === 0 ? 'ALL PASS' : 'FAILURES'} (${failures})`);
  process.exit(failures === 0 ? 0 : 1);
})().catch(e => { console.error('harness error:', e); process.exit(2); });

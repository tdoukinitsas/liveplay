// What is the engine's output latency, and does it hold up under load?
//
// Not a pass/fail harness — a measurement. It plays a growing number of files
// at once and reports, at each step, how much audio is queued ahead of the
// device (which IS the output latency), how long a render block actually takes
// against its budget, and whether anything underran.
//
// The question it exists to answer is the one the config cannot: the ring's
// depth is both the latency and the entire margin against a slow decode, since
// decoding runs on the render thread. Choosing a depth means knowing how close
// a render block comes to its deadline with a realistic number of cues open.
//
// The ramp is a short measurement per step, which is enough to see the shape
// but NOT enough to justify a latency default for software that has to survive
// a three-hour show. `soakSeconds` keeps everything playing and watches for
// the rare stall — a scheduler hiccup, a disk seek, an antivirus scan — that a
// two-second window will almost always miss. Those, not render-block time, are
// what a queue depth is really insuring against.
//
// usage: node latency-probe.js <port> <wavPath> [maxItems] [soakSeconds]
const PORT = process.argv[2], WAV = process.argv[3];
const MAX_ITEMS = Number(process.argv[4] || 16);
const SOAK_SECONDS = Number(process.argv[5] || 0);
const BASE = `http://127.0.0.1:${PORT}`;

const rest = async (p, o = {}) => {
  const r = await fetch(BASE + p, { headers: { 'content-type': 'application/json' }, ...o });
  const t = await r.text();
  try { return { status: r.status, body: JSON.parse(t) }; } catch { return { status: r.status, body: t }; }
};
const sleep = ms => new Promise(r => setTimeout(r, ms));

// Per-device drift telemetry.
//
// The engine renders at ONE rate — the clock device's — and resamples every
// other device so its queue stays at half depth. On a single-device machine
// this is one line saying [clock] and there is nothing else to see. With two
// devices open it is the whole feature, visible: the follower's fill settling
// near 50% and its ppm settling at a small steady figure means the loop has
// locked. A ppm parked near 2000 is the controller at its limit, which means
// whatever is wrong is not crystal drift.
const printDevices = s => {
  if (!Array.isArray(s.devices) || s.devices.length === 0) return;
  for (const d of s.devices) {
    // fill is the loop's own smoothed measurement — the figure the 50% target
    // refers to. ring is the raw occupancy beside it, which swings by a whole
    // device period and is here to catch a queue pinned at 0 or 100.
    console.log(`  device        ${d.isClock ? '[clock]' : '       '} ` +
                `fill ${d.fillPercent.toFixed(1).padStart(5)}%  ` +
                `ring ${d.ringFillPercent.toFixed(1).padStart(5)}%  ` +
                `${d.isClock ? '        -' : (d.ppm.toFixed(1) + ' ppm').padStart(9)}  ` +
                `${d.name}`);
  }
};

(async () => {
  // One bus, many items, all playing the same file. Same file deliberately:
  // each PlaybackItem owns its own decoder, so this measures N concurrent
  // decodes rather than N reads of one shared position.
  const items = Array.from({ length: MAX_ITEMS }, (_, i) => ({
    uuid: `lat-${String(i).padStart(3, '0')}`,
    type: 'audio', displayName: `Load ${i}`, mediaServerPath: WAV,
    volume: 0.1, endBehavior: 'loop',
  }));
  await rest('/api/project/document', {
    method: 'PUT', body: JSON.stringify({ name: 'latency', items }) });
  for (let i = 0; i < 200; i++) {
    const p = await rest('/api/project/progress');
    if (p.body && p.body.loading === false) break;
    await sleep(100);
  }

  const cfg = (await rest('/api/engine/stats')).body;
  console.log(`ring capacity   ${cfg.ringCapacityFrames} frames`);
  console.log(`device period   ${cfg.devicePeriodFrames} frames x ${cfg.devicePeriods}` +
              `  (${cfg.deviceMs.toFixed(1)} ms)`);
  console.log(`block budget    ${cfg.blockBudgetUs.toFixed(0)} us`);
  printDevices(cfg);
  console.log('');

  const header = 'playing   queued(ms)   render avg/max(us)   % of budget   underruns';
  console.log(header);
  console.log('-'.repeat(header.length));

  const rows = [];
  for (let n = 1; n <= MAX_ITEMS; n++) {
    await rest(`/api/project/items/${items[n - 1].uuid}/play`, {
      method: 'POST', body: '{}' });
    // Let the new item settle and the ring reach steady state before the
    // window opens; then reset the peak so each row measures only itself.
    await sleep(1500);
    await rest('/api/engine/stats?reset=1');
    await sleep(2500);
    const s = (await rest('/api/engine/stats')).body;
    const pct = (s.renderBlockUsMax / s.blockBudgetUs) * 100;
    rows.push({ n, ...s, pct });
    console.log(
      `${String(n).padStart(7)}   ${s.queuedMs.toFixed(1).padStart(10)}   ` +
      `${(s.renderBlockUsAvg.toFixed(0) + '/' + s.renderBlockUsMax.toFixed(0)).padStart(18)}   ` +
      `${pct.toFixed(1).padStart(11)}   ${String(s.underruns).padStart(9)}`);
  }

  if (SOAK_SECONDS > 0) {
    console.log(`\nsoaking ${SOAK_SECONDS}s with all ${MAX_ITEMS} playing...`);
    await rest('/api/engine/stats?reset=1');
    let worstQueue = Infinity;
    const t0 = Date.now();
    while ((Date.now() - t0) / 1000 < SOAK_SECONDS) {
      await sleep(500);
      const s = (await rest('/api/engine/stats')).body;
      // The shallowest the queue ever gets is the real safety margin: it is how
      // close the device came to running dry, which an average would hide.
      worstQueue = Math.min(worstQueue, s.queuedMs);
    }
    const s = (await rest('/api/engine/stats')).body;
    console.log(`  underruns          ${s.underruns}`);
    printDevices(s);
    console.log(`  worst render block ${s.renderBlockUsMax.toFixed(0)} us ` +
                `(${((s.renderBlockUsMax / s.blockBudgetUs) * 100).toFixed(1)}% of budget)`);
    console.log(`  shallowest queue   ${worstQueue.toFixed(1)} ms`);
    console.log(`  blocks rendered    ${s.blocksRendered}`);
  }

  for (const it of items) {
    await rest(`/api/project/items/${it.uuid}/stop`, { method: 'POST', body: '{}' });
  }

  const worst = rows.reduce((a, b) => (b.pct > a.pct ? b : a), rows[0]);
  const under = rows.reduce((a, b) => a + Number(b.underruns), 0);
  console.log(`\nworst render block: ${worst.renderBlockUsMax.toFixed(0)} us ` +
              `(${worst.pct.toFixed(1)}% of budget) at ${worst.n} files`);
  console.log(`total underruns:    ${under}`);
  console.log(`steady queue:       ${rows[rows.length - 1].queuedMs.toFixed(1)} ms`);
  printDevices((await rest('/api/engine/stats')).body);
})().catch(e => { console.error('probe error:', e); process.exit(2); });

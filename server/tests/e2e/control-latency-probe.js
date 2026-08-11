// How long does it take for a knob move to reach the engine?
//
// The output latency is one half of "I moved the EQ and heard it late"; this
// is the other. Every drag event on a knob fires POST /api/buses/<id>/dsp,
// fire-and-forget, at whatever rate the pointer moves. Two things could go
// wrong and they look identical to the operator:
//
//   1. each call is simply slow, adding a fixed lag
//   2. calls are issued faster than they complete, so they queue and the lag
//      GROWS through the gesture — the tell is that the last event lands long
//      after the pointer stopped
//
// So this fires at a realistic pointer rate for a realistic gesture length and
// reports both the per-call time and how far behind the last one finishes.
//
// usage: node control-latency-probe.js <port> [eventsPerSec] [seconds]
const PORT = process.argv[2];
const RATE = Number(process.argv[3] || 60);
const SECS = Number(process.argv[4] || 3);
const BASE = `http://127.0.0.1:${PORT}`;

const rest = async (p, o = {}) => {
  const r = await fetch(BASE + p, { headers: { 'content-type': 'application/json' }, ...o });
  const t = await r.text();
  try { return { status: r.status, body: JSON.parse(t) }; } catch { return { status: r.status, body: t }; }
};
const sleep = ms => new Promise(r => setTimeout(r, ms));

const pct = (arr, p) => {
  if (!arr.length) return 0;
  const s = [...arr].sort((a, b) => a - b);
  return s[Math.min(s.length - 1, Math.floor((p / 100) * s.length))];
};

(async () => {
  const bus = (await rest('/api/buses', {
    method: 'POST', body: JSON.stringify({ name: 'Latency', width: 2 }) })).body.id;

  const band = (freq) => ({
    eq: [
      { freq, gain: 6, q: 1, shelf: false, slope: 1 },
      { freq: 500,   gain: 0, q: 1,   shelf: false, slope: 1 },
      { freq: 2500,  gain: 0, q: 1,   shelf: false, slope: 1 },
      { freq: 10000, gain: 0, q: 0.7, shelf: false, slope: 1 },
    ],
  });

  // Warm the connection first. The first request to a fresh server pays for
  // TCP setup and route matching, which is not what a mid-gesture event costs.
  for (let i = 0; i < 5; i++) {
    await rest(`/api/buses/${bus}/dsp`, {
      method: 'POST', body: JSON.stringify(band(1000)) });
  }

  const total = Math.round(RATE * SECS);
  const interval = 1000 / RATE;
  const times = [];
  let done = 0;

  const gestureStart = Date.now();
  const pending = [];
  for (let i = 0; i < total; i++) {
    const freq = 200 + Math.round(1800 * (i / total));
    const t0 = Date.now();
    // Fire and forget, exactly as the panel does. Awaiting here would measure
    // a request/response ping-pong rather than a drag.
    pending.push(
      rest(`/api/buses/${bus}/dsp`, { method: 'POST', body: JSON.stringify(band(freq)) })
        .then(() => { times.push(Date.now() - t0); done++; })
        .catch(() => {}));
    await sleep(interval);
  }
  const gestureEnd = Date.now();
  await Promise.all(pending);
  const allDone = Date.now();

  console.log(`events            ${total} at ${RATE}/s over ${SECS}s`);
  console.log(`completed         ${done}`);
  console.log(`per-call ms       p50 ${pct(times, 50)}  p90 ${pct(times, 90)}  ` +
              `p99 ${pct(times, 99)}  max ${Math.max(...times)}`);
  // The number that separates "slow" from "backing up": if the queue keeps up,
  // the last call finishes within one call's time of the pointer stopping.
  console.log(`gesture took      ${gestureEnd - gestureStart} ms`);
  console.log(`last call landed  ${allDone - gestureEnd} ms after the pointer stopped`);

  await rest(`/api/buses/${bus}`, { method: 'DELETE' });
})().catch(e => { console.error('probe error:', e); process.exit(2); });

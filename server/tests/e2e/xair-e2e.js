// X Air plugin (#84) against a real Behringer X Air / Midas MR mixer.
//
// Fires every action through LivePlay's POST /api/events/test and reads the
// mixer back over OSC. Everything is put back as it was:
//   * faders and sends move only on the two strips you name, and never above
//     -20 dB (the test connection itself is capped there, too); the run stops
//     before touching anything if either strip is already above -20 dB;
//   * FX type, FX parameter and routing are written with the values the mixer
//     already has, which proves the encoding without changing anything;
//   * the recorder is only asked to record if no USB drive is in the mixer
//     (it must refuse); snapshot recall is NOT fired: it would overwrite the
//     whole desk.
//
// usage: node xair-e2e.js <port> <mixer ip> [strip A] [strip B]
//        strips default to ch/05 and ch/06, e.g. node xair-e2e.js 4517 192.168.1.105
const dgram = require('dgram');
const os = require('os');

const PORT = process.argv[2] || '4480';
const IP = process.argv[3];
const STRIP_A = process.argv[4] || 'ch/05';
const STRIP_B = process.argv[5] || 'ch/06';
const B = `http://127.0.0.1:${PORT}`;
if (!IP) { console.error('usage: node xair-e2e.js <port> <mixer ip> [strip A] [strip B]'); process.exit(2); }

const sleep = ms => new Promise(r => setTimeout(r, ms));
const f2db = f => (f >= 0.5 ? f * 40 - 30 : f >= 0.25 ? f * 80 - 50 : f >= 0.0625 ? f * 160 - 70 : f * 480 - 90);
let fails = 0;
const ok = (n, p, d = '') => { console.log(`${p ? 'PASS' : 'FAIL'}  ${n}  ${d}`); if (!p) fails++; };

// ---- minimal OSC ----
const pad = b => Buffer.concat([b, Buffer.alloc(4 - (b.length % 4))]);
const enc = addr => Buffer.concat([pad(Buffer.from(addr)), pad(Buffer.from(','))]);
function dec(buf) {
  let o = 0;
  const str = () => { const e = buf.indexOf(0, o); const s = buf.toString('utf8', o, e); o = (e + 4) & ~3; return s; };
  const addr = str(); const tags = o < buf.length ? str() : ',';
  const args = [];
  for (const t of tags.slice(1)) {
    if (t === 'i') { args.push(buf.readInt32BE(o)); o += 4; }
    else if (t === 'f') { args.push(+buf.readFloatBE(o).toFixed(4)); o += 4; }
    else if (t === 's') args.push(str());
  }
  return { addr, args };
}
// Bind on the mixer's subnet: through a VPN the reply may not come back otherwise.
function localFor(ip) {
  const n = s => s.split('.').reduce((a, x) => (a << 8) + (+x), 0) >>> 0;
  for (const a of Object.values(os.networkInterfaces()).flat())
    if (a.family === 'IPv4' && !a.internal && ((n(a.address) & n(a.netmask)) >>> 0) === ((n(ip) & n(a.netmask)) >>> 0))
      return a.address;
  return undefined;
}

const sock = dgram.createSocket('udp4');
const last = {};
sock.on('message', m => { const d = dec(m); last[d.addr] = d.args[0]; });
const get = async a => {
  delete last[a]; sock.send(enc(a), 10024, IP);
  for (let i = 0; i < 40 && !(a in last); i++) await sleep(10);
  return last[a];
};
const conn = cap => ({ id: 'draft', plugin: 'xair', config: { host: IP, port: 10024, maxLevelDb: cap } });
const fire = async (action, options, cap = -20) => {
  const r = await fetch(`${B}/api/events/test`, {
    method: 'POST', headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ event: { plugin: 'xair', action, options }, connection: conn(cap) }),
  });
  return r.json();
};

(async () => {
  await new Promise(r => sock.bind(0, localFor(IP), r));
  const fa = `/${STRIP_A}/mix/fader`, fb = `/${STRIP_B}/mix/fader`;
  const orig = {
    a: await get(fa), b: await get(fb), onA: await get(`/${STRIP_A}/mix/on`),
    snd: await get(`/${STRIP_A}/mix/03/level`), p41: await get('/fx/4/par/01'), t4: await get('/fx/4/type'),
    asrc: await get('/routing/aux/01/src'), apos: await get('/routing/aux/01/pos'), main: await get('/routing/main/01'),
    usb: await get('/-stat/usbmounted'),
  };
  if (orig.a === undefined) { console.log(`ABORT: no answer from the mixer at ${IP}`); process.exit(2); }
  const aDb = +f2db(orig.a).toFixed(2), bDb = +f2db(orig.b).toFixed(2);
  console.log(`${STRIP_A} ${aDb} dB, ${STRIP_B} ${bDb} dB`);
  if (aDb > -20 || bDb > -20) { console.log('ABORT: a test strip is above -20 dB; lower it first'); process.exit(2); }

  let r = await fire('check', {});
  ok('check connection', r.ok, JSON.stringify(r));

  r = await fire('fader', { strip: STRIP_A, level: -30, fade: 2 });
  const t0 = Date.now(), samples = [];
  while (Date.now() - t0 < 2400) { samples.push(+f2db(await get(fa)).toFixed(1)); await sleep(200); }
  const mono = samples.every((v, i) => i === 0 || v <= samples[i - 1] + 0.2);
  const end = samples[samples.length - 1];
  ok('2 s crossfade is gradual and lands on -30 dB', r.ok && mono && Math.abs(end + 30) < 0.3 &&
     samples.some(v => v < aDb - 1 && v > -29), samples.join(' '));
  await fire('fader', { strip: STRIP_A, level: aDb, fade: 1 }); await sleep(1300);
  ok('fade back to the original level', Math.abs(f2db(await get(fa)) - aDb) < 0.3);

  await fire('fader', { strip: STRIP_B, level: -5, fade: 0 }, Math.min(bDb, -21)); await sleep(150);
  const capped = f2db(await get(fb));
  ok('max level caps the fader', Math.abs(capped - Math.min(bDb, -21)) < 0.3, capped.toFixed(2));
  await fire('fader', { strip: STRIP_B, level: bDb, fade: 0 }); await sleep(150);
  ok(`${STRIP_B} restored`, Math.abs(f2db(await get(fb)) - bDb) < 0.3);

  await fire('mute', { strip: STRIP_A, state: 'mute' }); await sleep(120); const m = await get(`/${STRIP_A}/mix/on`);
  await fire('mute', { strip: STRIP_A, state: orig.onA ? 'unmute' : 'mute' }); await sleep(120);
  ok('mute, then restored', m === 0 && (await get(`/${STRIP_A}/mix/on`)) === orig.onA);

  const sndDb = orig.snd > 0 ? +f2db(orig.snd).toFixed(2) : -90;
  await fire('send', { strip: STRIP_A, send: '03', level: -30, fade: 0 }); await sleep(150);
  const sv = f2db(await get(`/${STRIP_A}/mix/03/level`));
  await fire('send', { strip: STRIP_A, send: '03', level: sndDb, fade: 0 }); await sleep(150);
  ok('send level set and restored', Math.abs(sv + 30) < 0.3 &&
     Math.abs((await get(`/${STRIP_A}/mix/03/level`)) - orig.snd) < 0.002);

  // Slot 4's parameter 1 in its own unit when it is a known effect; the
  // reading is put straight back.
  r = await fire('fx-type', { slot: 4, type: orig.t4 }); await sleep(120);
  ok('fx type (unchanged)', r.ok && (await get('/fx/4/type')) === orig.t4);
  r = await fire('fx-param', { slot: 4, param: 1, value: orig.t4 === 5 ? +(orig.p41 * 200).toFixed(3) : orig.p41 * 100, fade: 0 });
  await sleep(150);
  ok('fx parameter (unchanged)', r.ok && Math.abs((await get('/fx/4/par/01')) - orig.p41) < 0.002);

  r = await fire('route-output', { output: 'aux/01', source: orig.asrc, tap: String(orig.apos) }); await sleep(150);
  ok('route an output (unchanged)', r.ok && (await get('/routing/aux/01/src')) === orig.asrc &&
     (await get('/routing/aux/01/pos')) === orig.apos);
  r = await fire('route-main', { assign: orig.main }); await sleep(150);
  ok('route main (unchanged)', r.ok && (await get('/routing/main/01')) === orig.main);

  if (orig.usb === 0) {
    r = await fire('recorder', { command: 'record' });
    ok('recorder refuses without a USB drive', r.ok === false && /USB/.test(r.error || ''), r.error);
  } else {
    console.log('SKIP  recorder: a USB drive is in the mixer, and this test does not record');
  }
  r = await fire('fader', { strip: 'ch/99', level: -30 });
  ok('unknown strip rejected', r.ok === false, r.error);

  console.log(fails ? `\n${fails} FAILED` : '\nALL PASSED');
  sock.close(); process.exit(fails ? 1 : 0);
})().catch(e => { console.error(e); process.exit(2); });

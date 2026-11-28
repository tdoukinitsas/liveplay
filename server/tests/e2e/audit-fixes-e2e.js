// The 2.6.0 RC audit's server-side fixes (PR #93), against a server this suite
// owns (a fresh data folder, so nothing it does reaches a real show):
//   F-04  cart/<slot>/play takes the raw slot summary.cart reports, whatever
//         the page order; summary.cart names page, cart and address, and a
//         cart bound to a tab carries its name and plays while the tab does;
//   F-42  a slot no page shows can't be fired, and a refused binding says so;
//   F-18  a page removed through the API takes its carts (and pins) with it;
//   F-05  Select ▲/▼ keeps to one tab unless asked for the whole show;
//   F-36  Stop Current Cue stops the cue fired last;
//   F-17  stopping a cart bound to a tab stops the run it started, into the
//         next tab when the tab continues;
//   F-03  the cart keys have their own route, checked (F-25), and a cleared
//         cart takes its pin (F-13);
//   F-07  a show from before 2.6 keeps its 50 ms ducks; a 2.6 show keeps its
//   F-08  ramp times; either is saved as 2.6.0;
//   F-28  migrated HTTP actions encode a form body as 2.5 did, and
//   F-29  ones at the in-point become start events;
//   F-22  an event's stop-fade-mode change is live, not saved;
//   F-14  client views name a client, never its address;
//   F-20  a view name cut mid-character doesn't silence them.
// And the RC2 audit's:
//   R2-08 with two on air, GO and Up Next follow the cue fired first, or with
//         goContinuesFrom "latest" the one fired last; a re-press changes
//         nothing;
//   R2-13 a second Stop Current during a fade moves on, it doesn't cut;
//   R2-10 a 2.5 show's carts bound past slot 16 still show and fire, and a
//         slot no page shows can't be bound;
//   R2-11 a show saved by a 2.6 pre-release keeps its duck times.
//
// usage: node audit-fixes-e2e.js <port> [serverExe]   (never 4480)
const fs    = require('fs');
const os    = require('os');
const path  = require('path');
const dgram = require('dgram');
const { spawn } = require('child_process');

const PORT = process.argv[2];
if (!PORT || PORT === '4480') {
  console.error('usage: node audit-fixes-e2e.js <port other than 4480> [serverExe]');
  process.exit(2);
}
const EXE = path.resolve(process.argv[3] || path.join(__dirname, '../../build/Release/liveplay-server.exe'));
const BASE = `http://127.0.0.1:${PORT}`;

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
  while (body && body.pending && body.jobId) {
    await sleep(50);
    body = await (await fetch(`${BASE}/api/events/jobs/${body.jobId}`)).json();
  }
  return { status: r.status, body, text };
}
const post = (p, body = {}) => rest(p, { method: 'POST', body: JSON.stringify(body) });
const put  = (p, body) => rest(p, { method: 'PUT', body: JSON.stringify(body) });
const project = async () => { const r = await rest('/api/project'); return r.body.project || r.body; };
const summary = async () => (await rest('/api/state/summary')).body;
const onAir = async () => ((await summary()).playing || []).map(p => p.itemUuid);

function writeWav(file, seconds) {
  const rate = 48000, ch = 2, n = Math.floor(rate * seconds);
  const data = Buffer.alloc(n * ch * 2);
  const h = Buffer.alloc(44);
  h.write('RIFF', 0); h.writeUInt32LE(36 + data.length, 4); h.write('WAVE', 8);
  h.write('fmt ', 12); h.writeUInt32LE(16, 16); h.writeUInt16LE(1, 20); h.writeUInt16LE(ch, 22);
  h.writeUInt32LE(rate, 24); h.writeUInt32LE(rate * ch * 2, 28); h.writeUInt16LE(ch * 2, 32);
  h.writeUInt16LE(16, 34); h.write('data', 36); h.writeUInt32LE(data.length, 40);
  fs.writeFileSync(file, Buffer.concat([h, data]));
}

async function waitLoaded() {
  for (let i = 0; i < 100; i++) {
    const p = await rest('/api/project/progress');
    if (p.body && p.body.loading === false) break;
    await sleep(50);
  }
  await sleep(200);
}

(async () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'liveplay-audit-'));
  const dataDir = path.join(root, 'data');
  fs.mkdirSync(dataDir);
  const folder = root.replace(/\\/g, '/');
  const short = path.join(root, 'short.wav').replace(/\\/g, '/');
  const long = path.join(root, 'long.wav').replace(/\\/g, '/');
  writeWav(short, 0.3);
  writeWav(long, 8);

  const osc = [];
  const udp = dgram.createSocket('udp4');
  udp.on('message', m => osc.push(m.toString('utf8', 0, m.indexOf(0))));
  await new Promise(r => udp.bind(0, '127.0.0.1', r));
  const connection = { id: 'osc-1', name: 'OSC', plugin: 'osc', enabled: true,
                       config: { host: '127.0.0.1', port: udp.address().port } };
  const cue = (uuid, tabId, { media = long, end = 'nothing', extra = {} } = {}) => ({
    uuid, type: 'audio', displayName: uuid, mediaServerPath: media, ...(tabId ? { tabId } : {}),
    endBehavior: { action: end }, duckingBehavior: { mode: 'no-ducking' },
    playFade: 0, stopFade: 0, manualStopFade: 0, fadeOutDuration: 0,
    events: [{ id: `${uuid}-s`, trigger: 'start', plugin: 'osc', action: 'message',
               connectionId: 'osc-1', options: { address: `/start/${uuid}`, args: [] } }],
    ...extra,
  });
  const started = uuid => osc.includes(`/start/${uuid}`);

  const server = spawn(EXE, ['--port', PORT, '--bind', '127.0.0.1', '--data-dir', dataDir],
                       { stdio: 'ignore', detached: true, windowsHide: true });
  server.unref();

  try {
    for (let i = 0; i < 100; i++) {
      try { if ((await fetch(`${BASE}/api/health`)).ok) break; } catch { /* not yet */ }
      await sleep(100);
    }

    // ---- F-04 / F-42 / F-18: cart addressing ---------------------------------
    await put('/api/project/document', {
      name: 'carts', version: '2.6.0', busSchema: 2, folderPath: folder,
      eventConnections: [connection],
      playlistTabs: [{ id: 'ta', name: 'Act 1', atEnd: 'stop' }, { id: 'tb', name: 'Act 2', atEnd: 'wrap' }],
      cartPages: [{ id: 'p0', name: 'First', size: 16, slotBase: 0 },
                  { id: 'p1', name: 'Second', size: 16, slotBase: 64 }],
      items: [cue('a1', 'ta'), cue('a2', 'ta'), cue('b1', 'tb')],
      cartOnlyItems: [cue('alpha'), cue('charlie'), cue('hidden')],
      cartItems: [
        { slot: 0, itemUuid: 'alpha', index: [-1, 0] },
        { slot: 64, itemUuid: 'charlie', index: [-1, 64] },
        { slot: 1, itemUuid: 'tab:tb', index: [-1, 1] },
        { slot: 20, itemUuid: 'hidden', index: [-1, 20] },
      ],
    });
    await waitLoaded();
    // Page "Second" first in the strip.
    await put('/api/project/tabs', { cartPages: [
      { id: 'p1', name: 'Second', size: 16, slotBase: 64 }, { id: 'p0', name: 'First', size: 16, slotBase: 0 }] });
    let r = await post('/api/transport/cart/0/play');
    ok('cart/<slot>/play is the raw slot, whatever the page order (F-04)',
       r.status === 200 && r.body.uuid === 'alpha', JSON.stringify(r.body));
    r = await post('/api/transport/cart/64/play');
    ok('...including later pages', r.status === 200 && r.body.uuid === 'charlie', JSON.stringify(r.body));
    r = await post('/api/transport/cart/0/0/play');
    ok('page,cart follows the strip: page 0 is "Second"', r.status === 200 && r.body.uuid === 'charlie',
       JSON.stringify(r.body));
    r = await post('/api/transport/cart/64/stop');
    ok('cart/<slot>/stop exists and says it stopped something', r.status === 200 && r.body.stopped === true,
       JSON.stringify(r.body));
    await post('/api/transport/panic');
    await sleep(300);
    let s = await summary();
    const byUuid = Object.fromEntries((s.cart || []).map(c => [c.itemUuid, c]));
    ok('summary.cart names page, cart and address',
       byUuid.charlie?.page === 0 && byUuid.charlie?.cart === 0 && byUuid.charlie?.address === '0,0' &&
       byUuid.alpha?.page === 1 && byUuid.alpha?.slot === 0 && byUuid.alpha?.pageName === 'First',
       JSON.stringify(s.cart));
    ok("a slot no page shows isn't listed (F-42)", !byUuid.hidden, '');
    r = await post('/api/transport/cart/20/play');
    ok("...and can't be fired", r.status === 404, `HTTP ${r.status}`);
    r = await post('/api/project/cart', { slot: 99999, itemUuid: 'alpha' });
    ok('a refused binding says so', r.status === 400, `HTTP ${r.status}`);
    ok('a cart bound to a tab carries its name', byUuid['tab:tb']?.name === 'Act 2' &&
       byUuid['tab:tb']?.type === 'tab', JSON.stringify(byUuid['tab:tb']));
    await post('/api/transport/cart/1/play');
    await sleep(300);
    s = await summary();
    ok('...and plays while the tab does', (s.cart || []).find(c => c.itemUuid === 'tab:tb')?.playing === true,
       JSON.stringify((s.cart || []).find(c => c.itemUuid === 'tab:tb')));
    await post('/api/transport/panic');
    await sleep(300);

    // Cart keys (F-03, F-25, F-13).
    const patches = [];
    const ws = new WebSocket(`ws://127.0.0.1:${PORT}/ws`);
    ws.addEventListener('message', (e) => {
      try { const m = JSON.parse(typeof e.data === 'string' ? e.data : ''); if (m.type === 'doc_patch') patches.push(m); }
      catch { /* meters */ }
    });
    await new Promise(res => ws.addEventListener('open', res));
    r = await put('/api/project/cart-keys', { cartPinnedKeys: {
      0: { key: 'a', ctrlKey: false, shiftKey: false, altKey: false },
      64: { ctrlKey: true },                        // no key: dropped
      5: { key: 'b' },                              // no cart there: dropped
      alpha: { key: 'c' },                          // an RC uuid key: moved to its slot (taken, so...)
      1: { key: 'A', ctrlKey: false, shiftKey: false, altKey: false },   // the same key as slot 0: dropped
    }, cartSlotKeys: { 0: { key: 'q' }, 99: { key: 'w' } } });
    const pins = r.body.cartPinnedKeys || {};
    ok('pinned keys are checked: malformed, unbound and duplicate ones go (F-25)',
       Object.keys(pins).join(',') === '0' && pins['0'].key === 'a', JSON.stringify(pins));
    ok('position keys are checked too', Object.keys(r.body.cartSlotKeys || {}).join(',') === '0',
       JSON.stringify(r.body.cartSlotKeys));
    await sleep(150);
    ok('every client hears the change (F-03)', patches.some(p => p.op === 'cart_keys_changed'));
    await rest('/api/project/cart/0', { method: 'DELETE' });
    await sleep(100);
    r = await rest('/api/project/cart-keys');
    ok('a cleared cart takes its pin with it (F-13)', !('0' in (r.body.cartPinnedKeys || {})),
       JSON.stringify(r.body.cartPinnedKeys));
    // A whole-document save of this show doesn't undo it.
    r = await post('/api/project/save', { path: `${folder}/carts.liveplay`, document: {
      ...(await project()), cartPinnedKeys: { 0: { key: 'z' } }, cartPages: [], playlistTabs: [] } });
    r = await rest('/api/project/cart-keys');
    ok("another client's save can't overwrite the keys (F-03)", !('0' in (r.body.cartPinnedKeys || {})),
       JSON.stringify(r.body.cartPinnedKeys));
    r = await rest('/api/project/tabs');
    ok("...nor the tabs and pages (F-24)", r.body.cartPages?.length === 2 && r.body.playlistTabs?.length === 2,
       JSON.stringify(r.body).slice(0, 120));

    // F-18: removing page "Second" through the API takes Charlie with it.
    await put('/api/project/tabs', { cartPages: [{ id: 'p0', name: 'First', size: 16, slotBase: 0 }] });
    await put('/api/project/tabs', { cartPages: [{ id: 'p0', name: 'First', size: 16, slotBase: 0 },
                                                 { id: 'p2', name: 'New' }] });
    r = await rest('/api/cart/pages');
    const newPage = (r.body.pages || []).find(p => p.id === 'p2');
    ok("a removed page's carts don't reappear on the next new page (F-18)",
       newPage && newPage.carts.length === 0, JSON.stringify(newPage));

    // ---- F-05: selection keeps to a tab ----------------------------------------
    await post('/api/selection', { itemUuid: 'a2' });
    r = await post('/api/selection', { delta: 1 });
    ok('Select ▼ at the end of a tab stays there (F-05)', r.body.itemUuid === 'a2', JSON.stringify(r.body));
    r = await post('/api/selection', { delta: 1, scope: 'show' });
    ok('...unless the whole show is asked for', r.body.itemUuid === 'b1', JSON.stringify(r.body));
    await post('/api/selection', { itemUuid: 'a1' });
    r = await post('/api/selection', { delta: 1, tabId: 'tb' });
    ok("a step for a tab the selection isn't on starts at its top", r.body.itemUuid === 'b1',
       JSON.stringify(r.body));
    r = await post('/api/selection', { delta: 1, scope: 'sideways' });
    ok('an unknown scope is refused', r.status === 400, `HTTP ${r.status}`);

    // ---- F-36: Stop Current Cue --------------------------------------------------
    await post('/api/project/items/a1/play');
    await sleep(150);
    await post('/api/project/items/b1/play');
    await sleep(250);
    r = await post('/api/transport/stop_current', { fade_ms: 0 });
    await sleep(200);
    let air = await onAir();
    ok('Stop Current Cue stops the cue fired last (F-36)',
       r.body.uuid === 'b1' && air.includes('a1') && !air.includes('b1'), `${JSON.stringify(r.body)} on air: ${air}`);
    await post('/api/transport/panic');
    await sleep(300);
    r = await post('/api/transport/stop_current');
    ok('...and says so when nothing is playing', r.status === 404, `HTTP ${r.status}`);

    // ---- F-17: a tab cart stops its run --------------------------------------------
    await put('/api/project/document', {
      name: 'runs', version: '2.6.0', busSchema: 2, folderPath: folder,
      eventConnections: [connection],
      playlistTabs: [{ id: 'ta', name: 'Act 1', atEnd: 'continue' }, { id: 'tb', name: 'Act 2', atEnd: 'stop' }],
      items: [cue('r1', 'ta', { media: short, end: 'next' }), cue('r2', 'tb')],
      cartItems: [{ slot: 0, itemUuid: 'tab:ta', index: [-1, 0] }],
    });
    await waitLoaded();
    osc.length = 0;
    await post('/api/transport/cart/0/play');
    await sleep(1100);
    air = await onAir();
    ok('a continue tab runs into the next one', started('r1') && air.includes('r2'), `${osc} / ${air}`);
    r = await post('/api/transport/cart/0/0/stop');
    await sleep(250);
    air = await onAir();
    ok("stopping its cart stops the run, there too (F-17)", r.body.stopped === true && !air.includes('r2'),
       `${JSON.stringify(r.body)} on air: ${air}`);
    r = await post('/api/transport/cart/0/0/stop');
    ok('...and says when there was nothing to stop', r.body.stopped === false, JSON.stringify(r.body));

    // ---- Stop All during an end stop-fade doesn't advance (stability pass) ----------
    await put('/api/project/document', {
      name: 'stopfade', version: '2.6.0', busSchema: 2, folderPath: folder,
      eventConnections: [connection],
      settings: { stopAllFadeMs: 0 },
      items: [cue('sf1', null, { media: long, end: 'next',
                                 extra: { outPoint: 1.6, stopFade: 1.0, fadeOutDuration: 1.0 } }),
              cue('sf2')],
    });
    await waitLoaded();
    osc.length = 0;
    await post('/api/project/items/sf1/play');
    await sleep(900);   // inside its end fade (from 0.6 s)
    await post('/api/transport/stop_all', { fade_ms: 0 });
    await sleep(1600);
    air = await onAir();
    ok('Stop All during a cue\'s end fade does not start the next cue', started('sf1') && !started('sf2') &&
       !air.includes('sf2'), `${osc} / ${air}`);
    await post('/api/transport/panic');
    await sleep(300);

    // ---- A 2.5 action fires once per play, not on every loop ------------------------
    await put('/api/project/document', {
      name: 'loops', version: '2.0.0', busSchema: 2, folderPath: folder,
      eventConnections: [connection],
      items: [
        { ...cue('lp1', null, { media: short, end: 'loop' }),
          customActions: [{ timePoint: 0.1, action: { type: 'play-item', uuid: 'lp2' } }] },
        cue('lp2'),
      ],
    });
    await waitLoaded();
    osc.length = 0;
    await post('/api/project/items/lp1/play');
    await sleep(1400);   // several laps of 0.3 s
    await post('/api/transport/panic');
    await sleep(200);
    ok('a migrated 2.5 action fires once per play, not every loop',
       osc.filter(a => a === '/start/lp2').length === 1, osc.join(','));

    // ---- F-07 / F-08 / F-28 / F-29: a show from before 2.6 -------------------------
    await put('/api/project/document', {
      name: 'legacy', version: '2.0.0', busSchema: 2, folderPath: folder,
      items: [
        { ...cue('d1'), events: [], duckingBehavior: { mode: 'duck-others', duckLevel: 0.2, duckFadeIn: 0.25, duckFadeOut: 1.0 },
          inPoint: 2.0,
          customActions: [
            { timePoint: 1.0, action: { type: 'stop-all' } },
            { timePoint: 3.0, action: { type: 'http-request', request: {
                method: 'POST', url: 'http://127.0.0.1:9/form', body: { a: 'x y&z', 'b c': 1 } } } },
          ] },
      ],
    });
    await waitLoaded();
    let doc = await project();
    const d1 = doc.items.find(i => i.uuid === 'd1');
    ok('a pre-2.6 show keeps its 50 ms ducks (F-07)',
       d1.duckingBehavior.duckFadeIn === 0.05 && d1.duckingBehavior.duckFadeOut === 0.05,
       JSON.stringify(d1.duckingBehavior));
    ok('...and is a 2.6 show from now on (F-08)', doc.version === '2.6.0', doc.version);
    const stopEv = (d1.events || []).find(e => e.action === 'stop-all');
    const formEv = (d1.events || []).find(e => e.plugin === 'http');
    ok('an action at or before the in-point fires at the start (F-29)',
       stopEv?.trigger === 'start' && stopEv.time === undefined, JSON.stringify(stopEv));
    ok("a migrated Stop All fades each cue over its own Stop fade, as 2.5's did",
       stopEv?.options?.stopFade === 'cue', JSON.stringify(stopEv?.options));
    ok('...and the later action fires once per play', formEv?.once === true, JSON.stringify(formEv));
    ok('a form body is URL-encoded, as 2.5 sent it (F-28)',
       formEv?.options?.contentType === 'application/x-www-form-urlencoded' &&
       formEv?.options?.body === 'a=x+y%26z&b+c=1', JSON.stringify(formEv?.options));
    await put('/api/project/document', {
      name: 'new', version: '2.6.0', busSchema: 2, folderPath: folder,
      items: [{ ...cue('d2'), duckingBehavior: { mode: 'duck-others', duckLevel: 0.2, duckFadeIn: 0.25, duckFadeOut: 1.0 } }],
    });
    await waitLoaded();
    doc = await project();
    ok('a 2.6 show keeps its ramp times', doc.items[0].duckingBehavior.duckFadeIn === 0.25 &&
       doc.items[0].duckingBehavior.duckFadeOut === 1.0, JSON.stringify(doc.items[0].duckingBehavior));

    // ---- F-22: an event's stop-fade-mode is live, not saved --------------------------
    await post('/api/events/test', { event: { plugin: 'liveplay', action: 'stop-fade-mode', options: { mode: 'item' } } });
    doc = await project();
    ok('the event changes the live setting', doc.settings?.stopAllUsesItemFade === true,
       JSON.stringify(doc.settings?.stopAllUsesItemFade));
    const file = `${folder}/new.liveplay`;
    await post('/api/project/save', { path: file });
    const saved = JSON.parse(fs.readFileSync(file, 'utf8'));
    ok("...but the show is saved with the operator's (F-22)", saved.settings?.stopAllUsesItemFade !== true,
       JSON.stringify(saved.settings?.stopAllUsesItemFade));
    ok('the saved show says 2.6.0 (F-08)', saved.version === '2.6.0', saved.version);

    // ---- F-14 / F-20: client views ----------------------------------------------------
    patches.length = 0;
    ws.send(JSON.stringify({ type: 'set_view', playlistTab: 'é'.repeat(40), cartPage: 'x' }));
    await sleep(300);
    const views = [...patches].reverse().find(p => p.op === 'client_views');
    ok("a view name cut mid-character still reaches everyone (F-20)", !!views, '');
    const label = views?.views?.[0]?.label ?? '';
    ok('a client is named, never by its address (F-14)', /^Client \d+$/.test(label) && !label.includes('127.'),
       label);
    ws.close();

    // ---- R2-08: with two on air, GO follows a setting (default: fired first) ---------
    const plain = (uuid, extra = {}) => ({ ...cue(uuid, null, { end: 'next', extra }), events: [] });
    await put('/api/project/document', {
      name: 'go', version: '2.6.0', busSchema: 2, folderPath: folder,
      items: ['g1', 'g2', 'g3', 'g4', 'g5', 'g6', 'g7'].map(u => plain(u)),
    });
    await waitLoaded();
    await post('/api/project/items/g1/play'); await sleep(250);
    await post('/api/project/items/g5/play'); await sleep(250);
    s = await summary();
    ok('with two on air, Up Next follows the cue fired first, as in 2.5 (R2-08)', s.next?.itemUuid === 'g2',
       JSON.stringify(s.next));
    await post('/api/project/items/g1/play'); await sleep(150);   // already playing: changes nothing
    ok("...pressing it again while it plays doesn't move it in the firing order",
       (await summary()).next?.itemUuid === 'g2', JSON.stringify((await summary()).next));
    await rest('/api/project/settings', { method: 'PATCH', body: JSON.stringify({ goContinuesFrom: 'latest' }) });
    s = await summary();
    ok('...with goContinuesFrom "latest", the cue fired last', s.next?.itemUuid === 'g6', JSON.stringify(s.next));
    r = await post('/api/transport/go'); await sleep(250);
    ok('...and GO plays the same one', (await onAir()).includes('g6'), JSON.stringify(await onAir()));
    // The settings route drops a value its registry refuses (the response is
    // the settings map, so a refusal is the value not being there).
    r = await rest('/api/project/settings', { method: 'PATCH', body: JSON.stringify({ goContinuesFrom: 'sideways' }) });
    ok('an unknown choice is not stored', r.body?.goContinuesFrom === 'latest', JSON.stringify(r.body?.goContinuesFrom));
    await post('/api/transport/panic'); await sleep(300);

    // ---- R2-13: a second Stop Current moves on rather than cutting -------------------
    await put('/api/project/document', {
      name: 'stop-current', version: '2.6.0', busSchema: 2, folderPath: folder,
      items: ['s1', 's2'].map(u => ({ ...plain(u), endBehavior: { action: 'nothing' }, stopFade: 2, manualStopFade: 2 })),
    });
    await waitLoaded();
    await post('/api/project/items/s1/play'); await sleep(250);
    await post('/api/project/items/s2/play'); await sleep(250);
    r = await post('/api/transport/stop_current');
    ok('Stop Current stops the cue fired last', r.body?.uuid === 's2', JSON.stringify(r.body));
    await sleep(200);
    r = await post('/api/transport/stop_current');
    await sleep(150);
    const states = Object.fromEntries(((await summary()).playing || []).map(p => [p.itemUuid, p.transport]));
    ok("...pressed again during its fade, it moves on to the one before (R2-13)",
       r.body?.uuid === 's1' && states.s2 === 'fading_out', JSON.stringify({ r: r.body, states }));
    await post('/api/transport/panic'); await sleep(300);

    // ---- R2-10: a 2.5 show's API-bound carts past 16 keep working --------------------
    await put('/api/project/document', {
      name: 'old-carts', version: '2.0.0', busSchema: 2, folderPath: folder,
      items: [], cartOnlyItems: [plain('c20'), plain('c5')].map(c => ({ ...c, endBehavior: { action: 'nothing' } })),
      cartItems: [{ slot: 5, itemUuid: 'c5', index: [-1, 5] }, { slot: 20, itemUuid: 'c20', index: [-1, 20] }],
    });
    await waitLoaded();
    s = await summary();
    ok('a 2.5 show with a cart bound past 16 gets a page that shows it (R2-10)',
       (s.cart || []).some(c => c.slot === 20), JSON.stringify((s.cart || []).map(c => c.slot)));
    r = await post('/api/transport/cart/20/play');
    ok('...and it fires, as in 2.5', r.status === 200 && r.body.uuid === 'c20', JSON.stringify(r.body));
    await post('/api/transport/panic'); await sleep(200);
    r = await post('/api/project/cart', { slot: 60, itemUuid: 'c5' });
    ok("binding a slot no page shows is refused, not stored unseen (R2-10)", r.status === 400, `HTTP ${r.status}`);

    // ---- R2-11: a show saved by 2.6 RC1 ("2.0.0", but with 2.6 parts) ------------------
    await put('/api/project/document', {
      name: 'rc1', version: '2.0.0', busSchema: 2, folderPath: folder,
      playlistTabs: [{ id: 'ta', name: 'Act 1' }],
      items: [{ ...plain('r1'), tabId: 'ta',
                duckingBehavior: { mode: 'duck-others', duckLevel: 0.2, duckFadeIn: 3, duckFadeOut: 3 } }],
    });
    await waitLoaded();
    doc = await project();
    const r1 = doc.items.find(i => i.uuid === 'r1');
    ok('a show saved by a 2.6 pre-release keeps its duck times (R2-11)',
       r1?.duckingBehavior?.duckFadeIn === 3 && r1?.duckingBehavior?.duckFadeOut === 3,
       JSON.stringify(r1?.duckingBehavior));
  } catch (e) {
    ok('the suite ran to the end', false, e?.stack ?? String(e));
  } finally {
    try { process.kill(server.pid); } catch { /* gone */ }
    udp.close();
    await sleep(400);
    try { fs.rmSync(root, { recursive: true, force: true }); } catch { /* still closing */ }
  }

  console.log(failures ? `\n${failures} check(s) failed` : '\nAll checks passed');
  process.exit(failures ? 1 : 0);
})();

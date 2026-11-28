// Playlist tabs and cart pages (#74, #61) against a real server:
//   * a show from before them behaves exactly as it did, index API included;
//   * running order stays inside a tab unless the tab says otherwise;
//   * page,cart addressing, and a cart that starts a whole tab;
//   * tabs and pages survive save and load, and a save from a client that
//     doesn't know about them, but are never inherited by another show;
//   * every client hears about a change.
//
// usage: node tabs-e2e.js <port>     (never 4480: that may be the live app)
const fs    = require('fs');
const os    = require('os');
const path  = require('path');
const dgram = require('dgram');

const PORT = process.argv[2];
if (!PORT || PORT === '4480') {
  console.error('usage: node tabs-e2e.js <port other than 4480>');
  process.exit(2);
}
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
  return { status: r.status, body };
}
const post = (p, body = {}) => rest(p, { method: 'POST', body: JSON.stringify(body) });
const put  = (p, body) => rest(p, { method: 'PUT', body: JSON.stringify(body) });

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
  await sleep(150);
}

(async () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'liveplay-tabs-'));
  const folder = root.replace(/\\/g, '/');
  const short = path.join(root, 'short.wav');
  const long = path.join(root, 'long.wav');
  writeWav(short, 0.35);
  writeWav(long, 6);

  const osc = [];
  const udp = dgram.createSocket('udp4');
  udp.on('message', m => osc.push(m.toString('utf8', 0, m.indexOf(0))));
  await new Promise(r => udp.bind(0, '127.0.0.1', r));
  const connection = { id: 'osc-1', name: 'OSC', plugin: 'osc', enabled: true,
                       config: { host: '127.0.0.1', port: udp.address().port } };

  const patches = [];
  const ws = new WebSocket(`ws://127.0.0.1:${PORT}/ws`);
  ws.addEventListener('message', (e) => {
    try {
      const m = JSON.parse(typeof e.data === 'string' ? e.data : '');
      if (m.type === 'doc_patch') patches.push(m);
    } catch { /* binary meters */ }
  });
  await new Promise((resolve, reject) => {
    ws.addEventListener('open', resolve, { once: true });
    ws.addEventListener('error', reject, { once: true });
  });

  const cue = (uuid, tabId, action = 'nothing', file = short) => ({
    uuid, type: 'audio', displayName: uuid, mediaServerPath: file.replace(/\\/g, '/'),
    manualStopFade: 0, fadeOutDuration: 0, endBehavior: { action },
    ...(tabId ? { tabId } : {}),
    events: [{ id: `${uuid}-s`, trigger: 'start', plugin: 'osc', action: 'message',
               connectionId: 'osc-1', options: { address: `/start/${uuid}`, args: [] } }],
  });
  const started = uuid => osc.includes(`/start/${uuid}`);
  const lastArmed = () => [...patches].reverse().find(p => p.op === 'next_item_set')?.itemUuid;

  try {
    // ---- A show from before tabs --------------------------------------------
    await put('/api/project/document', {
      name: 'old', version: '2.0.0', busSchema: 2, folderPath: folder,
      eventConnections: [connection],
      items: [cue('old-1', null, 'next'), cue('old-2')],
      cartOnlyItems: [cue('cart-0')],
      cartItems: [{ slot: 0, itemUuid: 'cart-0', index: [-1, 0] }],
    });
    await waitLoaded();
    let r = await rest('/api/project/tabs');
    ok('a show from before tabs has none', r.body.playlistTabs?.length === 0 && r.body.cartPages?.length === 0);
    ok('the limits are published', r.body.limits?.maxCartPages === 100 && r.body.limits?.maxCartsPerPage === 64,
       JSON.stringify(r.body.limits));
    osc.length = 0;
    await post('/api/project/items/old-1/play');
    await sleep(900);
    ok('its running order still works', started('old-1') && started('old-2'), osc.join(' '));
    osc.length = 0;
    r = await post('/api/transport/cart/0/play');
    await sleep(150);
    ok('the old cart index still fires', r.status === 200 && started('cart-0'), `HTTP ${r.status}`);
    r = await post('/api/transport/cart/0/0/play');
    ok('and so does page 0, cart 0', r.status === 200, `HTTP ${r.status}`);
    r = await post('/api/transport/cart/1/0/play');
    ok('there is no page 1 yet', r.status === 404, `HTTP ${r.status}`);
    await post('/api/transport/panic');
    await sleep(250);

    // ---- Running order across tabs ------------------------------------------
    const tabs = [
      { id: 'wrap', name: 'Wrap', atEnd: 'wrap', color: '#ff0000' },
      { id: 'cont', name: 'Continue', atEnd: 'continue' },
      { id: 'halt', name: 'Stop', atEnd: 'stop' },
      { id: 'idle', name: 'Long', atEnd: 'wrap' },
    ];
    const pages = [
      { id: 'second', name: 'Stingers', size: 8, slotBase: 64, color: '#00ff00' },
      { id: 'first', name: 'Walk-in', size: 4, slotBase: 0 },
    ];
    const tabbedDoc = {
      name: 'tabbed', version: '2.0.0', busSchema: 2, folderPath: folder,
      eventConnections: [connection], playlistTabs: tabs, cartPages: pages,
      items: [
        cue('w1', 'wrap', 'next'), cue('w2', 'wrap'), cue('c1', 'cont', 'next'),
        cue('h1', 'halt'), cue('h2', 'halt'), cue('l1', 'idle', 'nothing', long), cue('l2', 'idle'),
      ],
      cartOnlyItems: [cue('page-cart', null, 'nothing', long)],
      cartItems: [
        { slot: 66, itemUuid: 'page-cart', index: [-1, 66] },
        { slot: 1, itemUuid: 'tab:idle', index: [-1, 1] },
      ],
    };
    await put('/api/project/document', tabbedDoc);
    await waitLoaded();

    osc.length = 0;
    await post('/api/project/items/w1/play');
    await sleep(1100);
    ok('next stays inside the tab', started('w1') && started('w2') && !started('c1'), osc.join(' '));
    await post('/api/transport/panic');
    await sleep(250);

    osc.length = 0;
    await post('/api/project/items/c1/play');
    await sleep(800);
    ok('a continue tab runs into the next tab', started('c1') && started('h1'), osc.join(' '));
    await post('/api/transport/panic');
    await sleep(250);

    patches.length = 0;
    await post('/api/project/items/w2/play');
    await sleep(800);
    ok('at the end of a wrap tab, Up Next is the top of that tab', lastArmed() === 'w1', String(lastArmed()));
    await post('/api/transport/panic');
    await sleep(250);

    ws.send(JSON.stringify({ type: 'set_next_item', item_uuid: '' }));
    await sleep(100);
    patches.length = 0;
    await post('/api/project/items/h2/play');
    await sleep(800);
    const armedAfterStop = lastArmed();
    ok('at the end of a stop tab, nothing new is armed', armedAfterStop === undefined || armedAfterStop === '',
       String(armedAfterStop));
    await post('/api/transport/panic');
    await sleep(250);

    // ---- Addressing ---------------------------------------------------------
    osc.length = 0;
    r = await post('/api/transport/cart/0/2/play');
    await sleep(200);
    ok('page,cart follows the page order, not the slot number',
       r.status === 200 && r.body.uuid === 'page-cart' && started('page-cart'), JSON.stringify(r.body));
    r = await post('/api/transport/cart/0/2/stop');
    ok('page,cart stop', r.status === 200, `HTTP ${r.status}`);
    r = await post('/api/transport/cart/0/8/play');
    ok('a cart past the size of the page does not exist', r.status === 404, `HTTP ${r.status}`);
    r = await post('/api/transport/cart/2/2');
    ok('an unknown route stays unknown', r.status === 404, `HTTP ${r.status}`);
    await sleep(150);

    osc.length = 0;
    r = await post('/api/transport/cart/1/1/play');
    await sleep(200);
    ok('a cart bound to a tab starts its first cue', r.status === 200 && started('l1'), osc.join(' '));
    const cueOnAir = async () => {
      const items = (await rest('/api/project/items?offset=0&limit=20')).body.items;
      const id = items.find(i => i.uuid === 'l1')?.cueId;
      return (await rest('/api/cues')).body.find(c => c.id === id);
    };
    r = await post('/api/transport/cart/1/1/stop');
    await sleep(300);
    const cueState = await cueOnAir();
    ok('and stops whatever of that tab is on air', r.status === 200 &&
       ['Stopped', 'stopped', 0].includes(cueState?.transport ?? cueState?.state ?? 'Stopped'),
       JSON.stringify(cueState?.transport ?? cueState?.state));

    r = await rest('/api/cart/pages');
    const list = r.body.pages ?? [];
    ok('the pages list is in tab order, with names', list[0]?.name === 'Stingers' && list[1]?.name === 'Walk-in',
       list.map(p => p.name).join(','));
    ok('...and each cart has its page,cart address', list[0]?.carts?.[0]?.address === '0,2' &&
       list[0]?.carts?.[0]?.name === 'page-cart', JSON.stringify(list[0]?.carts));
    ok('...and a tab cart is named after its tab', list[1]?.carts?.[0]?.name === 'Long',
       JSON.stringify(list[1]?.carts));

    // ---- Changing them ------------------------------------------------------
    patches.length = 0;
    r = await put('/api/project/tabs', { cartPages: [...pages, { id: 'third', name: 'New', size: 999 }] });
    await sleep(150);
    const third = r.body.cartPages?.find(p => p.id === 'third');
    ok('a new page gets a free block and a legal size', third?.size === 64 && third?.slotBase === 128,
       JSON.stringify(third));
    ok('the playlist tabs were left alone', r.body.playlistTabs?.length === 4);
    ok('every client is told', patches.some(p => p.op === 'tabs_changed' && p.cartPages?.length === 3));
    r = await put('/api/project/tabs', { cartPages: 'nope' });
    ok('a malformed list is refused', r.status === 400, `HTTP ${r.status}`);
    await put('/api/project/tabs', { cartPages: pages });

    // ---- Saving -------------------------------------------------------------
    const file = path.join(root, 'tabbed.liveplay').replace(/\\/g, '/');
    await post('/api/project/save', { path: file });
    const saved = JSON.parse(fs.readFileSync(file, 'utf8'));
    ok('tabs and pages are saved', saved.playlistTabs?.length === 4 && saved.cartPages?.length === 2);
    ok('each cue keeps its tab', saved.items?.find(i => i.uuid === 'c1')?.tabId === 'cont');
    r = await post('/api/project/load', { path: file });
    await waitLoaded();
    r = await rest('/api/project/header');
    const header = r.status === 200 ? r.body : (await rest('/api/project')).body;
    ok('and come back on load', header.playlistTabs?.length === 4 && header.cartPages?.length === 2);

    const { playlistTabs: _t, cartPages: _p, ...fromOlderClient } = tabbedDoc;
    await put('/api/project/document', fromOlderClient);
    await waitLoaded();
    r = await rest('/api/project/tabs');
    ok('a save from a client that predates tabs keeps them',
       r.body.playlistTabs?.length === 4 && r.body.cartPages?.length === 2);
    await put('/api/project/document', { ...fromOlderClient, playlistTabs: [], cartPages: [] });
    await waitLoaded();
    r = await rest('/api/project/tabs');
    ok('a client that does know can remove them', r.body.playlistTabs?.length === 0 && r.body.cartPages?.length === 0);

    await put('/api/project/document', tabbedDoc);
    await waitLoaded();
    await put('/api/project/document', {
      name: 'another', version: '2.0.0', busSchema: 2,
      folderPath: path.join(root, 'elsewhere').replace(/\\/g, '/'), items: [],
    });
    await waitLoaded();
    r = await rest('/api/project/tabs');
    ok('another show never inherits them', r.body.playlistTabs?.length === 0 && r.body.cartPages?.length === 0);
  } catch (e) {
    ok('the suite ran to the end', false, e?.stack ?? String(e));
  } finally {
    await post('/api/transport/panic').catch(() => {});
    await post('/api/project/close').catch(() => {});
    ws.close();
    udp.close();
    await sleep(400);
    try { fs.rmSync(root, { recursive: true, force: true }); } catch { /* handles closing */ }
  }

  console.log(failures ? `\n${failures} check(s) failed` : '\nAll checks passed');
  process.exit(failures ? 1 : 0);
})();

// Reproduces the log the maintainer pasted: every POST /api/project/save
// re-materialised every bus and opened more audio devices, which is what the
// "Ring underrun" storm was.
//
// usage: node save-churn.js <port> <wav> <projDir> <serverLog>
const fs = require('fs');
const PORT = process.argv[2], WAV = process.argv[3], DIR = process.argv[4];
const LOG  = process.argv[5];
const countInLog = (needle) => {
  try {
    return fs.readFileSync(LOG, 'utf8').split('\n')
             .filter(l => l.includes(needle)).length;
  } catch { return -1; }
};
const BASE = `http://127.0.0.1:${PORT}`;
const rest = async (p, o = {}) => {
  const r = await fetch(BASE + p, { headers: { 'content-type': 'application/json' }, ...o });
  const t = await r.text();
  try { return { status: r.status, body: JSON.parse(t) }; } catch { return { status: r.status, body: t }; }
};
const sleep = ms => new Promise(r => setTimeout(r, ms));
const uuid = 'item-churn-0001';

let failures = 0;
const ok = (name, pass, detail = '') => {
  console.log(`${pass ? 'PASS' : 'FAIL'}  ${name}${detail ? '   ' + detail : ''}`);
  if (!pass) failures++;
};

(async () => {
  await rest('/api/project/document', {
    method: 'PUT',
    body: JSON.stringify({
      name: 'churn', folderPath: DIR,
      settings: { previewDevice: '' },
      items: [{ uuid, type: 'audio', displayName: 'Probe', mediaServerPath: WAV,
                volume: 1, endBehavior: 'stop' }],
    }),
  });
  for (let i = 0; i < 60; i++) {
    const p = await rest('/api/project/progress');
    if (p.body && p.body.loading === false) break;
    await sleep(100);
  }

  // A bus bound to real hardware, which is what makes a rebuild open devices.
  const devs = (await rest('/api/devices')).body;
  const devName = devs[0].display_name || devs[0].name;
  await rest('/api/outputs', {
    method: 'PUT',
    body: JSON.stringify({ version: 1, outputs: [
      { name: 'FOH', channels: [{ device: devName, hwChannel: 0 },
                                { device: devName, hwChannel: 1 }] }]}),
  });
  const bus = (await rest('/api/buses', {
    method: 'POST',
    body: JSON.stringify({ name: 'Churn', width: 2,
                           output: { type: 'output', target: 'FOH' } }),
  })).body.id;
  await rest(`/api/project/items/${uuid}`, {
    method: 'PATCH', body: JSON.stringify({ busId: bus }),
  });
  await sleep(500);

  const doc = (await rest('/api/project')).body;
  const item = doc.items.find(i => i.uuid === uuid);
  const devsBefore  = countInLog('Opened audio device');
  const matBefore   = countInLog('materialise_buses:');
  const underBefore = countInLog('Ring underrun');

  // Ten ordinary property saves, exactly as handleSave() issues them: a whole
  // document snapshot with items and NO `buses` key.
  for (let n = 0; n < 10; n++) {
    await rest('/api/project/save', {
      method: 'POST',
      body: JSON.stringify({
        path: DIR + '/churn.liveplay',
        document: {
          name: doc.name, version: doc.version, folderPath: DIR,
          items: [{ ...item, volume: 0.5 + n * 0.01 }],
          cartItems: doc.cartItems ?? [], cartOnlyItems: doc.cartOnlyItems ?? [],
          theme: doc.theme, settings: doc.settings,
        },
      }),
    });
    await sleep(120);
  }
  await sleep(400);
  const devsAfter  = countInLog('Opened audio device');
  const matAfter   = countInLog('materialise_buses:');
  const underAfter = countInLog('Ring underrun');

  ok('ten saves open no new audio devices',
     devsAfter === devsBefore, `${devsBefore} -> ${devsAfter} opens logged`);
  ok('ten saves do not re-materialise the buses',
     matAfter === matBefore, `${matBefore} -> ${matAfter} materialise passes`);
  ok('ten saves cause no ring underruns',
     underAfter === underBefore, `${underBefore} -> ${underAfter} underruns`);

  const s = (await rest('/api/buses')).body;
  const stillThere = s.find(b => b.id === bus);
  ok('the bus survives ten saves', !!stillThere, stillThere ? stillThere.id : '(gone)');
  ok('the item is still assigned to it',
     !!stillThere && (stillThere.itemUuids || []).includes(uuid));
  ok('the bus is still wired to hardware', !!stillThere && stillThere.bound === true);

  // And a genuine bus change must still rebuild.
  await rest(`/api/buses/${bus}`, {
    method: 'PATCH', body: JSON.stringify({ name: 'Churn Renamed' }),
  });
  await sleep(300);
  const renamed = (await rest('/api/buses')).body.find(b => b.id === bus);
  ok('a real bus edit still takes effect', renamed && renamed.name === 'Churn Renamed',
     renamed ? renamed.name : '(gone)');

  console.log(`\n${failures === 0 ? 'ALL PASS' : 'FAILURES'} (${failures})`);
  process.exit(failures === 0 ? 0 : 1);
})();

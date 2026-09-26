// Picking a device for a bus must not put a sound-card name in the show file
// as a bare device reference.
//
// Round 2's D29 gave the strip's output picker a plain "Devices" group, and
// picking one wrote {type:"output", target:"<device name>"} straight into the
// document, resolved at play time by OutputMap's identity fallback. That
// reopened the leak the logical-output map had just closed: email the
// .liveplay elsewhere and it names a sound card, and — worse —
// open_device_by_name() falls back to the DEFAULT device when the name matches
// nothing, so a sub-mix quietly lands in the house.
//
// The fix keeps the affordance and changes what it writes: a target naming a
// device present on this machine is materialised into outputs.json as a
// logical output of that name. The document still says "Scarlett 2i2", but as
// a logical output the map really carries — visible and re-pointable in the
// output map, and plainly unmapped at a venue that has no such device.
//
// This is server-side on purpose: it must hold for Companion and curl too, not
// only for the strip's picker.
//
//   node server/tests/e2e/output-materialise-e2e.js 4500
//
// Mutates server config: it writes outputs.json next to the binary, and puts
// the original map back at the end.
const PORT = process.argv[2];
const BASE = `http://127.0.0.1:${PORT}`;
let failures = 0;
const ok = (n, p, d = '') => { console.log(`${p ? 'PASS' : 'FAIL'}  ${n}${d ? '   ' + d : ''}`); if (!p) failures++; };
const rest = async (p, o = {}) => {
  const r = await fetch(BASE + p, { headers: { 'content-type': 'application/json' }, ...o });
  const t = await r.text();
  try { return { status: r.status, body: JSON.parse(t) }; } catch { return { status: r.status, body: t }; }
};
const outputs = async () => (await rest('/api/outputs')).body;
const buses   = async () => (await rest('/api/buses')).body;
const named   = (map, n) => (map.outputs ?? []).find(o => o.name === n);

(async () => {
  // The map as we found it, restored at the end — scripts run back to back
  // against one server and this one writes to outputs.json.
  const original = await outputs();
  const restore = async () => {
    await rest('/api/outputs', {
      method: 'PUT',
      body: JSON.stringify({ version: original.version ?? 1, outputs: original.outputs ?? [] }),
    });
  };

  try {
    const devices = (await rest('/api/devices')).body;
    if (!Array.isArray(devices) || devices.length === 0) {
      console.log('SKIP  no audio devices on this host — nothing to materialise against');
      process.exit(0);
    }
    // A real device name, and one that is definitely not a device.
    const realDevice = devices[0].name ?? devices[0].display_name;
    const ghost      = 'No Such Interface 9f3c2a';
    ok('the host reports at least one device', !!realDevice, String(realDevice));

    // A project with one ordinary bus to point around.
    await rest('/api/project/document', {
      method: 'PUT',
      body: JSON.stringify({
        name: 'materialiseProbe', busSchema: 2, items: [],
        buses: [{ id: 'sub', name: 'Sub', width: 2, gainDb: 0,
                  output: { type: 'output', target: 'Main Out' } }],
      }),
    });

    // Make sure the map does not already carry the device name, or the test
    // would pass without the code doing anything.
    let map = await outputs();
    if (named(map, realDevice)) {
      await rest('/api/outputs', {
        method: 'PUT',
        body: JSON.stringify({
          version: map.version ?? 1,
          outputs: (map.outputs ?? []).filter(o => o.name !== realDevice),
        }),
      });
      map = await outputs();
    }
    ok('the device name is not in the map to start with', !named(map, realDevice));

    // ---- Targeting a present device materialises it --------------------
    let r = await rest('/api/buses/sub', {
      method: 'PATCH',
      body: JSON.stringify({ output: { type: 'output', target: realDevice } }),
    });
    ok('the patch is accepted', r.status === 200, `status ${r.status}`);

    map = await outputs();
    const entry = named(map, realDevice);
    ok('the device is now a logical output in the map', !!entry,
       JSON.stringify((map.outputs ?? []).map(o => o.name)));
    ok('mapped stereo onto that device, hw 0/1',
       !!entry && entry.channels?.length === 2 &&
       entry.channels[0].device === realDevice && entry.channels[0].hwChannel === 0 &&
       entry.channels[1].device === realDevice && entry.channels[1].hwChannel === 1,
       JSON.stringify(entry?.channels));

    let b = (await buses()).find(x => x.id === 'sub');
    ok('the bus still targets that name', b?.output?.target === realDevice,
       JSON.stringify(b?.output));
    ok('and it reads as bound', b?.bound === true, String(b?.bound));

    // ---- Idempotent ------------------------------------------------------
    const before = JSON.stringify((await outputs()).outputs);
    r = await rest('/api/buses/sub', {
      method: 'PATCH',
      body: JSON.stringify({ output: { type: 'output', target: realDevice } }),
    });
    const after = JSON.stringify((await outputs()).outputs);
    ok('re-picking the same device changes nothing', r.status === 200 && before === after,
       before === after ? '(map unchanged)' : 'map changed');

    // ---- A name that is not a device is left alone -----------------------
    // The identity fallback still covers it, and inventing a map entry for
    // hardware that is not here would be a lie about the machine.
    r = await rest('/api/buses/sub', {
      method: 'PATCH',
      body: JSON.stringify({ output: { type: 'output', target: ghost } }),
    });
    map = await outputs();
    ok('a target that names no present device is not materialised',
       r.status === 200 && !named(map, ghost),
       JSON.stringify((map.outputs ?? []).map(o => o.name)));
    b = (await buses()).find(x => x.id === 'sub');
    ok('and that bus reports itself unbound', b?.bound === false, String(b?.bound));

    // ---- Built-ins keep their own meaning --------------------------------
    // "Main Out" unmapped means the platform default device (D26), so an entry
    // written for it would replace that meaning with whatever device happened
    // to share the name. Asserted as "the map does not move", not "the name is
    // absent": this host may legitimately already map Main Out, and absence
    // would then be untestable rather than true.
    //
    // The built-in exclusion is belt-and-braces either way — reaching it needs
    // a device literally called "Main Out", which cannot be arranged here — so
    // what this pins is the outcome, which is the part that matters.
    const beforeBuiltin = JSON.stringify((await outputs()).outputs);
    r = await rest('/api/buses/sub', {
      method: 'PATCH',
      body: JSON.stringify({ output: { type: 'output', target: 'Main Out' } }),
    });
    const afterBuiltin = JSON.stringify((await outputs()).outputs);
    ok('targeting a built-in leaves the map untouched',
       r.status === 200 && beforeBuiltin === afterBuiltin,
       beforeBuiltin === afterBuiltin ? '(map unchanged)' : `${beforeBuiltin} -> ${afterBuiltin}`);
  } finally {
    await restore();
  }

  console.log(`\n${failures === 0 ? 'ALL PASS' : 'FAILURES'} (${failures})`);
  process.exit(failures === 0 ? 0 : 1);
})().catch(async e => { console.error('harness error:', e); process.exit(2); });

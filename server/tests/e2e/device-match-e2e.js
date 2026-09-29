// Device-name matching, end-to-end against a running server.
//
//   A. A bus that names a device the way it was called before Windows
//      renumbered it ("OUT 1-2 (BEHRINGER…)" for what is now
//      "OUT 1-2 (2- BEHRINGER…)") is bound, and reports the real device in
//      outputDevices. It used to be unbound and silent.
//   B. An output-map entry whose device is absent is NOT bound and resolves to
//      nothing. It used to report bound and play into the default device.
//
// A needs a present device whose name carries a "(N- " renumbering prefix;
// on a machine without one it is skipped, not failed. The output map is
// saved first and restored at the end — this writes outputs.json.
//
// usage: node device-match-e2e.js <port>
const PORT = process.argv[2] || '4480';
const BASE = `http://127.0.0.1:${PORT}`;
let failures = 0;
const ok = (name, pass, detail) => {
  console.log(`${pass ? 'PASS' : 'FAIL'}  ${name}   ${detail}`);
  if (!pass) failures++;
};
async function rest(p, opts = {}) {
  const r = await fetch(BASE + p, { headers: { 'content-type': 'application/json' }, ...opts });
  const text = await r.text();
  let body; try { body = JSON.parse(text); } catch { body = text; }
  return { status: r.status, body };
}
const sleep = ms => new Promise(r => setTimeout(r, ms));

(async () => {
  await rest('/api/project/document', { method: 'PUT', body: JSON.stringify({ name: 'device-match-e2e', items: [] }) });
  await sleep(500);
  const savedMap = (await rest('/api/outputs')).body;
  const restore = () => rest('/api/outputs', { method: 'PUT',
    body: JSON.stringify({ version: savedMap.version ?? 1, outputs: savedMap.outputs ?? [] }) });

  try {
    const devices = (await rest('/api/devices')).body;
    const renumbered = devices.find(d => /\(\d+- /.test(d.display_name) || /^\d+- /.test(d.display_name));
    const mk = async name => (await rest('/api/buses', { method: 'POST', body: JSON.stringify({ name, width: 2 }) })).body.id;
    const bus = async id => (await rest(`/api/buses/${id}`)).body;

    // ---- A. renumbered device ---------------------------------------------
    if (!renumbered) {
      console.log('SKIP  A: no device with a "(N- " prefix on this machine');
    } else {
      const real  = renumbered.display_name;
      const stale = real.replace(/\((\d+)- /, '(').replace(/^\d+- /, '');
      const id = await mk('Stale target');
      const r = await rest(`/api/buses/${id}`, { method: 'PATCH',
        body: JSON.stringify({ output: { type: 'output', target: stale } }) });
      const b = await bus(id);
      ok('a bus naming the pre-renumbering device name is accepted', r.status === 200, `status ${r.status}`);
      ok('...and is bound', b.bound === true, `"${stale}" bound=${b.bound}`);
      ok('...and resolves to the real device', JSON.stringify(b.outputDevices) === JSON.stringify([real]),
         JSON.stringify(b.outputDevices));
      const map = (await rest('/api/outputs')).body;
      const entry = (map.outputs || []).find(o => o.name === stale);
      ok('a materialised map entry records the real device, not the stale name',
         !entry || entry.channels.every(c => c.device === real), JSON.stringify(entry));
      await rest(`/api/buses/${id}`, { method: 'DELETE' });
    }

    // ---- B. mapping to an absent device -----------------------------------
    await rest('/api/outputs', { method: 'PUT', body: JSON.stringify({ version: 1, outputs: [
      ...(savedMap.outputs || []),
      { name: 'Ghost Out', channels: [{ device: 'No Such Card 9000', hwChannel: 0 },
                                      { device: 'No Such Card 9000', hwChannel: 1 }] },
    ] }) });
    const gid = await mk('Ghost bus');
    await rest(`/api/buses/${gid}`, { method: 'PATCH', body: JSON.stringify({ output: { type: 'output', target: 'Ghost Out' } }) });
    const g = await bus(gid);
    ok('a map entry whose device is absent is not bound', g.bound === false, `bound=${g.bound}`);
    ok('...and resolves to no device (not the default)', Array.isArray(g.outputDevices) && g.outputDevices.length === 0,
       JSON.stringify(g.outputDevices));
    ok('...and holds no master pair', g.masters === null, JSON.stringify(g.masters));
    await rest(`/api/buses/${gid}`, { method: 'DELETE' });

    // ---- Main Out unmapped is still the default device --------------------
    const master = (await rest('/api/buses')).body.find(x => x.master);
    if (master && master.output.target === 'Main Out' && !(savedMap.outputs || []).some(o => o.name === 'Main Out')) {
      ok('Main Out unmapped still reaches the default device', master.bound && master.outputDevices.includes(''),
         JSON.stringify(master.outputDevices));
    }
  } finally {
    await restore();
    await rest('/api/project/close', { method: 'POST', body: '{}' });
  }
  console.log(failures ? `\nFAILURES: ${failures}` : '\nALL PASS');
  process.exit(failures ? 1 : 0);
})().catch(e => { console.error(e); process.exit(2); });

// replace_full_document() now skips materialise_buses() when the bus list is
// unchanged, which is what stops an ordinary save from tearing down every
// strip. The risk is skipping when it should NOT: opening a different project
// must still rebuild, or you get the previous project's routing.
const PORT = process.argv[2];
const BASE = `http://127.0.0.1:${PORT}`;
let failures = 0;
const ok = (n, p, d = '') => { console.log(`${p ? 'PASS' : 'FAIL'}  ${n}${d ? '   ' + d : ''}`); if (!p) failures++; };
const rest = async (p, o = {}) => {
  const r = await fetch(BASE + p, { headers: { 'content-type': 'application/json' }, ...o });
  const t = await r.text();
  try { return { status: r.status, body: JSON.parse(t) }; } catch { return { status: r.status, body: t }; }
};
const buses = async () => (await rest('/api/buses')).body;

(async () => {
  // A project that names its own buses, as a .liveplay on disk does.
  await rest('/api/project/document', {
    method: 'PUT',
    body: JSON.stringify({
      name: 'projA', items: [],
      buses: [
        { id: 'foh',   name: 'FOH',   width: 2, gainDb: 0, output: { type: 'master', target: '' } },
        { id: 'comms', name: 'Comms', width: 1, gainDb: 0, output: { type: 'master', target: '' } },
      ],
    }),
  });
  let b = await buses();
  const a = b.filter(x => !x.system).map(x => x.id).sort();
  ok('project A materialised its own buses', JSON.stringify(a) === '["comms","foh"]', a.join(','));
  ok('every bus in A has a strip', b.every(x => !!x.mixerId));

  // A DIFFERENT project. If the skip fires here, we keep A's buses.
  await rest('/api/project/document', {
    method: 'PUT',
    body: JSON.stringify({
      name: 'projB', items: [],
      buses: [
        { id: 'stage', name: 'Stage', width: 2, gainDb: 0, output: { type: 'master', target: '' } },
      ],
    }),
  });
  b = await buses();
  const c = b.filter(x => !x.system).map(x => x.id).sort();
  ok('project B replaced them, not kept A', JSON.stringify(c) === '["stage"]', c.join(','));
  ok('every bus in B has a strip', b.every(x => !!x.mixerId));

  // And a save-shaped round trip (no buses key) must NOT disturb them.
  const before = (await buses()).map(x => x.id + ':' + x.mixerId).sort().join('|');
  await rest('/api/project/document', {
    method: 'PUT', body: JSON.stringify({ name: 'projB', items: [] }),
  });
  const after = (await buses()).map(x => x.id + ':' + x.mixerId).sort().join('|');
  ok('a save-shaped round trip keeps the same strips', before === after,
     before === after ? '(strip ids unchanged)' : `${before}  ->  ${after}`);

  console.log(`\n${failures === 0 ? 'ALL PASS' : 'FAILURES'} (${failures})`);
  process.exit(failures === 0 ? 0 : 1);
})().catch(e => { console.error('harness error:', e); process.exit(2); });

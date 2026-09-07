// The filesystem allow-list and the configurable CORS origin.
//
// Before these, /api/fs/list with an empty path enumerated every drive,
// /api/metadata read any absolute path, /api/fs/mkdir created directories
// anywhere, and every response carried Access-Control-Allow-Origin: * — so a
// page on any site the operator visited could drive all of it cross-origin.
//
// Unlike the other harnesses here this one starts its OWN servers, because
// what it tests is boot configuration: one confined to a temp root, one left
// at the defaults. The second half matters as much as the first — the defaults
// are deliberately the pre-2.5 behaviour so an upgrade does not break an
// install whose shows live on another volume, and a test that only proved the
// jail works would not notice if the default had silently become restrictive.
//
//   node server/tests/e2e/fs-jail-e2e.js [path-to-liveplay-server]
const { spawn } = require('child_process');
const fs   = require('fs');
const os   = require('os');
const path = require('path');

const EXE = process.argv[2] || path.join('server', 'build', 'Release',
  process.platform === 'win32' ? 'liveplay-server.exe' : 'liveplay-server');

const JAILED_PORT = 4551;
const OPEN_PORT   = 4552;

let failures = 0;
const ok = (n, p, d = '') => { console.log(`${p ? 'PASS' : 'FAIL'}  ${n}${d ? '   ' + d : ''}`); if (!p) failures++; };

const rest = async (port, p, o = {}) => {
  const r = await fetch(`http://127.0.0.1:${port}${p}`,
    { headers: { 'content-type': 'application/json' }, ...o });
  const t = await r.text();
  let body; try { body = JSON.parse(t); } catch { body = t; }
  return { status: r.status, body, cors: r.headers.get('access-control-allow-origin') };
};

const sleep = ms => new Promise(r => setTimeout(r, ms));

async function waitForHealth(port, tries = 40) {
  for (let i = 0; i < tries; i++) {
    try {
      const r = await fetch(`http://127.0.0.1:${port}/api/health`);
      if (r.ok) return true;
    } catch { /* not up yet */ }
    await sleep(250);
  }
  return false;
}

(async () => {
  const root    = fs.mkdtempSync(path.join(os.tmpdir(), 'liveplay-jail-'));
  const inside  = path.join(root, 'inside');
  fs.mkdirSync(inside);
  fs.writeFileSync(path.join(inside, 'marker.txt'), 'hello');

  // Somewhere real, outside the root, that definitely exists.
  const outside = os.homedir();

  const procs = [];
  const start = (port, extra) => {
    const p = spawn(EXE, ['--port', String(port), '--bind', '127.0.0.1', ...extra],
                    { stdio: 'ignore' });
    procs.push(p);
    return p;
  };

  try {
    start(JAILED_PORT, ['--fs-root', root, '--cors-origin', 'https://booth.example']);
    start(OPEN_PORT, []);

    if (!await waitForHealth(JAILED_PORT)) { console.error('jailed server never came up'); process.exit(2); }
    if (!await waitForHealth(OPEN_PORT))   { console.error('open server never came up');   process.exit(2); }

    // ---- The jail holds ------------------------------------------------
    let r = await rest(JAILED_PORT, `/api/fs/list?path=${encodeURIComponent(inside)}&filter=all`);
    ok('a path inside the root is listed', r.status === 200, `status ${r.status}`);
    ok('and its contents come back',
       r.status === 200 && JSON.stringify(r.body.entries ?? []).includes('marker.txt'),
       JSON.stringify(r.body.entries ?? []).slice(0, 80));

    r = await rest(JAILED_PORT, `/api/fs/list?path=${encodeURIComponent(outside)}&filter=all`);
    ok('a path outside the root is refused', r.status === 403, `status ${r.status}`);

    // The traversal case: a path that starts inside and climbs out. This is
    // the one a lexical prefix check without weakly_canonical would let past.
    const climb = path.join(inside, '..', '..', '..');
    r = await rest(JAILED_PORT, `/api/fs/list?path=${encodeURIComponent(climb)}&filter=all`);
    ok('a traversal out of the root is refused', r.status === 403, `${climb} -> ${r.status}`);

    // A sibling whose name merely starts with the root's, which a naive
    // startsWith would admit.
    const sibling = root + 'X';
    fs.mkdirSync(sibling, { recursive: true });
    r = await rest(JAILED_PORT, `/api/fs/list?path=${encodeURIComponent(sibling)}&filter=all`);
    ok('a sibling sharing the root name prefix is refused', r.status === 403,
       `${sibling} -> ${r.status}`);

    // Unique per run, and removed afterwards. A fixed name would be created
    // for real by any build where the guard is broken — including the
    // deliberately-broken one used to prove this assertion can fail — and would
    // then keep this check red on every later run against a correct build.
    const forbiddenDir = path.join(outside, `liveplay-should-not-exist-${process.pid}-${Date.now()}`);
    r = await rest(JAILED_PORT, '/api/fs/mkdir', {
      method: 'POST', body: JSON.stringify({ path: forbiddenDir }),
    });
    ok('mkdir outside the root is refused', r.status === 403, `status ${r.status}`);
    ok('and it created nothing', !fs.existsSync(forbiddenDir), forbiddenDir);
    try { fs.rmSync(forbiddenDir, { recursive: true, force: true }); } catch {}

    r = await rest(JAILED_PORT, `/api/metadata?path=${encodeURIComponent(path.join(outside, 'anything.mp3'))}`);
    ok('metadata outside the root is refused', r.status === 403, `status ${r.status}`);

    r = await rest(JAILED_PORT, '/api/project/load', {
      method: 'POST', body: JSON.stringify({ path: path.join(outside, 'x.liveplay') }),
    });
    ok('loading a project outside the root is refused', r.status === 403, `status ${r.status}`);

    r = await rest(JAILED_PORT, '/api/project/save', {
      method: 'POST', body: JSON.stringify({ path: path.join(outside, 'x.liveplay') }),
    });
    ok('saving a project outside the root is refused', r.status === 403, `status ${r.status}`);

    // With a jail configured the computer root IS the jail — offering Home and
    // every drive letter would just list places the next call 403s on.
    r = await rest(JAILED_PORT, '/api/fs/list?path=&filter=all');
    const names = (r.body.entries ?? []).map(e => e.full_path);
    ok('the computer root lists only the configured roots',
       r.status === 200 && names.length === 1, JSON.stringify(names));

    // ---- CORS is what was configured ------------------------------------
    r = await rest(JAILED_PORT, '/api/health');
    ok('the configured CORS origin is sent', r.cors === 'https://booth.example', String(r.cors));

    // ---- The defaults did not change -------------------------------------
    r = await rest(OPEN_PORT, `/api/fs/list?path=${encodeURIComponent(outside)}&filter=all`);
    ok('unconfigured, a path outside is still reachable', r.status === 200, `status ${r.status}`);
    r = await rest(OPEN_PORT, '/api/fs/list?path=&filter=all');
    ok('unconfigured, the computer root still enumerates drives',
       r.status === 200 && (r.body.entries ?? []).length > 0,
       `${(r.body.entries ?? []).length} entries`);
    r = await rest(OPEN_PORT, '/api/health');
    ok('unconfigured, CORS is still *', r.cors === '*', String(r.cors));

    try { fs.rmSync(sibling, { recursive: true, force: true }); } catch {}
  } finally {
    for (const p of procs) { try { p.kill(); } catch {} }
    try { fs.rmSync(root, { recursive: true, force: true }); } catch {}
  }

  console.log(`\n${failures === 0 ? 'ALL PASS' : 'FAILURES'} (${failures})`);
  process.exit(failures === 0 ? 0 : 1);
})().catch(e => { console.error('harness error:', e); process.exit(2); });

// The failures an event system can add to a running show, against a server this
// suite starts and owns, so a crash is seen as a crash:
//   * opening another show must not fire the old show's end events into it;
//   * two cues whose markers play each other must not restart for ever, while
//     cues that start each other when they END keep running as a playback order;
//   * a start-behaviour cycle must not overflow the stack;
//   * "nan" and "inf" option strings must not reach a fader;
//   * a mistyped setting in a show file must not end the process;
//   * a Test against a dead host must not stall other requests;
//   * fades and media swaps racing each other must not corrupt anything;
//   * quitting with an event in flight must exit cleanly and stay down.
//
// usage: node stability-e2e.js <port> [serverExe] [soakSeconds]
//        (never 4480: that may be the live app)
const fs    = require('fs');
const os    = require('os');
const net   = require('net');
const path  = require('path');
const dgram = require('dgram');
const { spawn, execFile } = require('child_process');

const PORT = process.argv[2];
if (!PORT || PORT === '4480') {
  console.error('usage: node stability-e2e.js <port other than 4480> [serverExe] [soakSeconds]');
  process.exit(2);
}
// Resolved, because `start` on Windows reads a forward slash as a switch.
const EXE = path.resolve(process.argv[3] || path.join(__dirname, '../../build/Release/liveplay-server.exe'));
const SOAK_SEC = Number(process.argv[4] || 30);
const BASE = `http://127.0.0.1:${PORT}`;

let failures = 0;
const ok = (name, pass, detail = '') => {
  console.log(`${pass ? 'PASS' : 'FAIL'}  ${name}   ${detail}`);
  if (!pass) failures++;
};
const sleep = ms => new Promise(r => setTimeout(r, ms));

async function raw(p, opts = {}) {
  const r = await fetch(BASE + p, { headers: { 'content-type': 'application/json' }, ...opts });
  const text = await r.text();
  let body; try { body = JSON.parse(text); } catch { body = text; }
  return { status: r.status, body };
}
async function rest(p, opts = {}) {
  const r = await raw(p, opts);
  while (r.body && r.body.pending && r.body.jobId) {
    await sleep(50);
    r.body = (await raw(`/api/events/jobs/${r.body.jobId}`)).body;
  }
  return r;
}
const post = (p, body = {}) => rest(p, { method: 'POST', body: JSON.stringify(body) });
const fire = (action, options) => post('/api/events/test',
  { event: { plugin: 'liveplay', action, options } });
const healthy = async () => {
  try { return (await raw('/api/health')).status === 200; } catch { return false; }
};

function writeWav(file, seconds) {
  const rate = 48000, ch = 2, n = Math.floor(rate * seconds);
  const data = Buffer.alloc(n * ch * 2);
  for (let i = 0; i < n; i++) {
    const v = Math.round(Math.sin(2 * Math.PI * 440 * i / rate) * 0.1 * 32767);
    data.writeInt16LE(v, i * 4); data.writeInt16LE(v, i * 4 + 2);
  }
  const h = Buffer.alloc(44);
  h.write('RIFF', 0); h.writeUInt32LE(36 + data.length, 4); h.write('WAVE', 8);
  h.write('fmt ', 12); h.writeUInt32LE(16, 16); h.writeUInt16LE(1, 20); h.writeUInt16LE(ch, 22);
  h.writeUInt32LE(rate, 24); h.writeUInt32LE(rate * ch * 2, 28); h.writeUInt16LE(ch * 2, 32);
  h.writeUInt16LE(16, 34); h.write('data', 36); h.writeUInt32LE(data.length, 40);
  fs.writeFileSync(file, Buffer.concat([h, data]));
}

const oscAddress = (m) => m.toString('utf8', 0, m.indexOf(0));

async function waitLoaded() {
  for (let i = 0; i < 100; i++) {
    const p = await raw('/api/project/progress');
    if (p.body && p.body.loading === false) return;
    await sleep(50);
  }
}
const openDoc = async (doc) => {
  const r = await rest('/api/project/document', { method: 'PUT', body: JSON.stringify(doc) });
  await waitLoaded();
  await sleep(150);
  return r;
};
const audio = (uuid, file, extra = {}) => ({
  uuid, type: 'audio', displayName: uuid, mediaServerPath: file.replace(/\\/g, '/'),
  manualStopFade: 0, fadeOutDuration: 0, endBehavior: { action: 'nothing' }, ...extra,
});
const playItem = uuid => post(`/api/project/items/${uuid}/play`);

// Launched the way the app launches it. On Windows that means a console window
// of its own, because closing that window is what a clean stop goes through.
async function startServer(dataDir, logFile) {
  const pidFile = path.join(dataDir, 'pid.json');
  const args = ['--port', PORT, '--bind', '127.0.0.1', '--data-dir', dataDir, '--pidfile', pidFile];
  if (process.platform === 'win32') {
    spawn('cmd.exe', ['/c', 'start', '', '/D', path.dirname(EXE), EXE, ...args],
          { stdio: 'ignore', detached: true, windowsHide: false }).unref();
  } else {
    const log = fs.openSync(logFile, 'a');
    spawn(EXE, args, { detached: true, stdio: ['ignore', log, log] }).unref();
  }
  for (let i = 0; i < 100; i++) {
    try { return JSON.parse(fs.readFileSync(pidFile, 'utf8')).pid; } catch { await sleep(100); }
  }
  throw new Error('the server never wrote its pidfile');
}
const isAlive = (pid) => { try { process.kill(pid, 0); return true; } catch { return false; } };
const logsText = (dataDir, logFile) => {
  const files = [logFile];
  const dir = path.join(dataDir, 'logs');
  if (fs.existsSync(dir)) files.push(...fs.readdirSync(dir).map(f => path.join(dir, f)));
  return files.filter(f => fs.existsSync(f)).map(f => fs.readFileSync(f, 'utf8')).join('\n');
};
// A clean stop. On Windows, node's kill() is a hard terminate and taskkill can't
// politely close a console process, so this joins the server's console and
// sends the Ctrl+Break its handler shuts down on.
const askToQuit = (pid) => new Promise((resolve) => {
  if (process.platform !== 'win32') {
    try { process.kill(pid, 'SIGTERM'); } catch { /* gone */ }
    resolve();
    return;
  }
  const script = `
    Add-Type -Namespace LivePlayTest -Name Console -MemberDefinition '
      [DllImport("kernel32.dll")] public static extern bool FreeConsole();
      [DllImport("kernel32.dll")] public static extern bool AttachConsole(uint id);
      [DllImport("kernel32.dll")] public static extern bool SetConsoleCtrlHandler(IntPtr h, bool add);
      [DllImport("kernel32.dll")] public static extern bool GenerateConsoleCtrlEvent(uint ev, uint group);
    '
    [LivePlayTest.Console]::FreeConsole() | Out-Null
    if (-not [LivePlayTest.Console]::AttachConsole(${Number(pid)})) { exit 1 }
    [LivePlayTest.Console]::SetConsoleCtrlHandler([IntPtr]::Zero, $true) | Out-Null
    [LivePlayTest.Console]::GenerateConsoleCtrlEvent(1, 0) | Out-Null
  `;
  execFile('powershell.exe', ['-NoProfile', '-NonInteractive', '-Command', script],
           { windowsHide: true }, () => resolve());
});
const crashLogs = (dirs) => dirs.flatMap((d) => {
  const logs = path.join(d, 'crash-logs');
  return fs.existsSync(logs) ? fs.readdirSync(logs).map(f => path.join(logs, f)) : [];
});

(async () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'liveplay-stability-'));
  const dataDir = path.join(root, 'data');
  const show = path.join(root, 'show');
  fs.mkdirSync(dataDir);
  fs.mkdirSync(path.join(show, 'media'), { recursive: true });
  const logFile = path.join(root, 'server.log');
  const long = path.join(show, 'media', 'long.wav');
  const shortWav = path.join(show, 'media', 'short.wav');
  const alt = path.join(show, 'media', 'alt.wav');
  writeWav(long, 8); writeWav(shortWav, 0.4); writeWav(alt, 6);

  const osc = [];
  const udp = dgram.createSocket('udp4');
  udp.on('message', m => osc.push(oscAddress(m)));
  await new Promise(r => udp.bind(0, '127.0.0.1', r));
  const connection = { id: 'osc-1', name: 'OSC', plugin: 'osc', enabled: true,
                       config: { host: '127.0.0.1', port: udp.address().port } };
  const oscEvent = (id, trigger, address, extra = {}) => ({
    id, trigger, plugin: 'osc', action: 'message', connectionId: 'osc-1',
    options: { address, args: [] }, ...extra,
  });
  const playEvent = (id, trigger, target, extra = {}) => ({
    id, trigger, plugin: 'liveplay', action: 'play-item', options: { item: target }, ...extra,
  });
  const count = address => osc.filter(a => a === address).length;

  // A host that accepts a connection and then never says anything.
  const held = [];
  const blackHole = net.createServer((s) => { held.push(s); s.on('error', () => {}); });
  await new Promise(r => blackHole.listen(0, '127.0.0.1', r));
  const holePort = blackHole.address().port;

  const before = crashLogs([dataDir, path.dirname(EXE)]);
  const serverPid = await startServer(dataDir, logFile);
  let exited = false;
  const watchExit = setInterval(() => { if (!isAlive(serverPid)) exited = true; }, 100);

  try {
    for (let i = 0; i < 80 && !(await healthy()); i++) await sleep(100);
    ok('the server starts', await healthy());

    // ---- Opening another show ----------------------------------------------
    await openDoc({ name: 'first', folderPath: show.replace(/\\/g, '/'),
                    eventConnections: [connection],
                    items: [audio('old-cue', long, { events: [oscEvent('o-end', 'end', '/old/end')] })] });
    await playItem('old-cue');
    await sleep(400);
    const nextFile = path.join(show, 'next.liveplay');
    fs.writeFileSync(nextFile, JSON.stringify({
      name: 'second', version: '2.0.0', busSchema: 2, eventConnections: [connection],
      items: [audio('new-cue', shortWav)],
    }));
    osc.length = 0;
    const loaded = await post('/api/project/load', { path: nextFile.replace(/\\/g, '/') });
    await waitLoaded();
    await sleep(500);
    ok('the next show opens', loaded.status === 200, `HTTP ${loaded.status}`);
    ok("the old show's end event does not fire into it", count('/old/end') === 0,
       `${count('/old/end')} fired`);

    // ---- Markers that play each other --------------------------------------
    await openDoc({ name: 'pingpong', folderPath: show.replace(/\\/g, '/'),
      eventConnections: [connection], items: [
        audio('pp-a', long, { events: [oscEvent('a-s', 'start', '/pp/a'),
                                       playEvent('a-m', 'time', 'pp-b', { time: 0.05 })] }),
        audio('pp-b', long, { events: [oscEvent('b-s', 'start', '/pp/b'),
                                       playEvent('b-m', 'time', 'pp-a', { time: 0.05 })] }),
      ] });
    osc.length = 0;
    await playItem('pp-a');
    await sleep(1500);
    ok('a marker loop starts each cue once', count('/pp/a') === 1 && count('/pp/b') === 1,
       `a=${count('/pp/a')} b=${count('/pp/b')}`);
    await post('/api/transport/stop_all');
    await sleep(200);

    // ---- End events are a playback order, and must keep going --------------
    await openDoc({ name: 'order', folderPath: show.replace(/\\/g, '/'),
      eventConnections: [connection], items: [
        audio('or-a', shortWav, { events: [oscEvent('a-s', 'start', '/or/a'),
                                           playEvent('a-e', 'end', 'or-b')] }),
        audio('or-b', shortWav, { events: [oscEvent('b-s', 'start', '/or/b'),
                                           playEvent('b-e', 'end', 'or-a')] }),
      ] });
    osc.length = 0;
    await playItem('or-a');
    await sleep(2200);
    ok('cues chained by END events keep alternating', count('/or/a') >= 2 && count('/or/b') >= 2,
       `a=${count('/or/a')} b=${count('/or/b')}`);
    await post('/api/transport/panic');
    await sleep(300);

    // ---- A start-behaviour cycle -------------------------------------------
    await openDoc({ name: 'cycle', folderPath: show.replace(/\\/g, '/'), items: [
      audio('sb-a', long, { startBehavior: { action: 'play', targetUuid: 'sb-b' } }),
      audio('sb-b', long, { startBehavior: { action: 'play', targetUuid: 'sb-a' } }),
      audio('sb-c', long, { startBehavior: { action: 'play', targetUuid: 'sb-c' } }),
    ] });
    await playItem('sb-a');
    await playItem('sb-c');
    await sleep(300);
    ok('a start-behaviour cycle does not bring the server down', !exited && await healthy());
    await post('/api/transport/stop_all');

    // ---- Bad numbers --------------------------------------------------------
    await post('/api/master/gain', { db: -3 });
    let bad = await fire('master-level', { level: 'nan' });
    await sleep(100);
    let db = (await raw('/api/master/gain')).body.db;
    ok('"nan" is refused and the fader stays put', bad.body.ok === false && Math.abs(db + 3) < 0.1,
       `${JSON.stringify(bad.body)} at ${db} dB`);
    bad = await fire('master-level', { level: -6, fade: 'inf' });
    await sleep(200);
    db = (await raw('/api/master/gain')).body.db;
    ok('an "inf" fade is refused and the fader stays put',
       bad.body.ok === false && Math.abs(db + 3) < 0.1, `${JSON.stringify(bad.body)} at ${db} dB`);
    const good = await fire('master-level', { level: '-6' });
    await sleep(100);
    db = (await raw('/api/master/gain')).body.db;
    ok('a number written as text still works', good.body.ok === true && Math.abs(db + 6) < 0.1, String(db));
    await post('/api/master/gain', { db: 0 });

    // ---- A mistyped setting in a show file ---------------------------------
    const badFile = path.join(show, 'bad.liveplay');
    fs.writeFileSync(badFile, JSON.stringify({
      name: 'bad', version: '2.0.0', busSchema: 2,
      settings: { disableLimiter: 'yes' }, items: [audio('bad-cue', shortWav)],
    }));
    await post('/api/project/load', { path: badFile.replace(/\\/g, '/') });
    await waitLoaded();
    await sleep(300);
    ok('a mistyped setting does not end the process', !exited && await healthy());

    // ---- A Test against a host that never answers --------------------------
    const started = await raw('/api/events/test', { method: 'POST', body: JSON.stringify({
      event: { plugin: 'http', action: 'request', options: { method: 'GET', path: '/' } },
      connection: { id: 'draft', plugin: 'http',
                    config: { baseUrl: `http://127.0.0.1:${holePort}`, timeoutMs: 1500 } },
    }) });
    ok('a slow Test answers straight away with a job',
       started.body?.pending === true && !!started.body?.jobId, JSON.stringify(started.body));
    let worst = 0;
    for (let i = 0; i < 12; i++) {
      const results = await Promise.all(Array.from({ length: 16 }, async () => {
        const t = performance.now();
        await raw('/api/health');
        return performance.now() - t;
      }));
      worst = Math.max(worst, ...results);
      await sleep(40);
    }
    ok('other requests are not held up while it waits', worst < 400, `worst ${worst.toFixed(0)} ms`);
    let settled = { pending: true };
    for (let i = 0; i < 100 && settled.pending; i++) {
      await sleep(50);
      settled = (await raw(`/api/events/jobs/${started.body.jobId}`)).body;
    }
    ok('the job reports the failure when it ends', settled.ok === false && !!settled.error,
       JSON.stringify(settled));
    const again = await raw(`/api/events/jobs/${started.body.jobId}`);
    ok('a collected job is gone', again.status === 404, `HTTP ${again.status}`);

    // ---- Soak: fades, swaps and show changes racing each other --------------
    const soakDoc = {
      name: 'soak', folderPath: show.replace(/\\/g, '/'), eventConnections: [connection],
      items: ['s1', 's2', 's3', 's4'].map((u, i) => audio(u, i % 2 ? alt : long, {
        events: [oscEvent(`${u}-s`, 'start', `/soak/${u}`),
                 oscEvent(`${u}-m`, 'time', `/soak/${u}/m`, { time: 0.1 }),
                 oscEvent(`${u}-e`, 'end', `/soak/${u}/e`)],
      })),
    };
    await openDoc(soakDoc);
    const uuids = ['s1', 's2', 's3', 's4'];
    const pick = () => uuids[Math.floor(Math.random() * uuids.length)];
    const soakEnd = Date.now() + SOAK_SEC * 1000;
    let actions = 0;
    let lastShowChange = Date.now();
    while (Date.now() < soakEnd && !exited) {
      const u = pick();
      const jobs = [
        playItem(u),
        fire('duck-item', { item: pick(), level: -12, fade: 0.3 }),
        fire('pause-item', { item: pick() }),
        fire('resume-item', { item: pick() }),
        rest(`/api/project/items/${pick()}`, { method: 'PATCH', body: JSON.stringify({
          mediaServerPath: (Math.random() < 0.5 ? alt : long).replace(/\\/g, '/'),
        }) }),
        post(`/api/project/items/${pick()}/stop`),
      ];
      await Promise.allSettled(jobs);
      actions += jobs.length;
      if (Date.now() - lastShowChange > 4000) {
        lastShowChange = Date.now();
        await openDoc(soakDoc);
      }
      await sleep(25);
    }
    ok(`survives ${SOAK_SEC} s of racing fades, swaps and show changes`,
       !exited && await healthy(), `${actions} actions`);
    await post('/api/transport/panic');

    // ---- Quit with an event in flight ---------------------------------------
    await openDoc({ name: 'quit', folderPath: show.replace(/\\/g, '/'),
      eventConnections: [{ id: 'http-1', name: 'Dead', plugin: 'http', enabled: true,
        config: { baseUrl: `http://127.0.0.1:${holePort}`, timeoutMs: 2000 } }],
      items: [audio('quit-cue', long, { events: [{
        id: 'q', trigger: 'start', plugin: 'http', action: 'request', connectionId: 'http-1',
        options: { method: 'GET', path: '/' } }] })] });
    await playItem('quit-cue');
    await sleep(300);
    await askToQuit(serverPid);
    for (let i = 0; i < 150 && !exited; i++) await sleep(100);
    ok('it exits while an event is in flight', exited);
    await sleep(3000);
    ok('and stays down, with no crash restart', !(await healthy()));
    const logText = logsText(dataDir, logFile);
    ok('the shutdown ran to the end', logText.includes('Bye.'));
    const after = crashLogs([dataDir, path.dirname(EXE), show]);
    const fresh = after.filter(f => !before.includes(f));
    ok('no crash log was written', fresh.length === 0, fresh.join(', '));
  } catch (e) {
    ok('the suite ran to the end', false, e?.stack ?? String(e));
  } finally {
    clearInterval(watchExit);
    if (isAlive(serverPid)) { try { process.kill(serverPid); } catch { /* gone */ } }
    udp.close();
    held.forEach(s => s.destroy());
    blackHole.close();
    await sleep(300);
    if (failures) console.log(`server log kept at ${logFile}`);
    else { try { fs.rmSync(root, { recursive: true, force: true }); } catch { /* in use */ } }
  }

  console.log(failures ? `\n${failures} check(s) failed` : '\nAll checks passed');
  process.exit(failures ? 1 : 0);
})();

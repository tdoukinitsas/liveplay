const { app, BrowserWindow, ipcMain, dialog, shell, Menu, protocol, screen } = require('electron');
const { autoUpdater } = require('electron-updater');
const path = require('path');
const fs = require('fs');
const { spawn, exec } = require('child_process');
const express = require('express');
const youtubesearchapi = require('youtube-search-api');
const YTDlpWrap = require('yt-dlp-wrap').default;
const ffmpeg = require('fluent-ffmpeg');
const { promisify } = require('util');
const https = require('https');
const execPromise = promisify(exec);

let ffmpegPath = null;
let ffmpegAvailable = false;

// ===========================================================================
// LivePlay C++ server lifecycle
// ---------------------------------------------------------------------------
// When the user runs the desktop client in "local" server mode, Electron
// spawns the bundled liveplay-server binary as a *detached* child process —
// it survives renderer reloads and Electron quits. A lockfile records the
// PID + port so the next launch can reattach to the running instance
// instead of spawning a duplicate (which would clash on the same port).
//
// Users can explicitly shut the server down via the
// `liveplay-server:shutdown` IPC handle. Config lives in
// <userData>/liveplay-server.json; lock in <userData>/liveplay-server.lock.
// ===========================================================================
const LIVEPLAY_DEFAULT_PORT = 4480;
const LIVEPLAY_CONFIG_FILENAME = 'liveplay-server.json';
const LIVEPLAY_LOCK_FILENAME   = 'liveplay-server.lock';
// liveplayServerProc is the spawned ChildProcess when *this* renderer
// instance started the server. After a reattach on a fresh launch we may
// only have a PID — see liveplayServerPid below.
let liveplayServerProc = null;
let liveplayServerPid  = null;
let liveplayServerPort = LIVEPLAY_DEFAULT_PORT;
let liveplayServerExitTimer = null;
// `--silent` on the app's own command line: launch the local server into the
// system tray for this session, whatever Settings says.
const launchedSilent = process.argv.includes('--silent');

function liveplayConfigPath() {
  return path.join(app.getPath('userData'), LIVEPLAY_CONFIG_FILENAME);
}
function liveplayLockPath() {
  return path.join(app.getPath('userData'), LIVEPLAY_LOCK_FILENAME);
}

function readLiveplayLock() {
  try {
    const raw = fs.readFileSync(liveplayLockPath(), 'utf-8');
    const j = JSON.parse(raw);
    if (Number.isInteger(j.pid) && Number.isInteger(j.port)) return j;
  } catch {}
  return null;
}

function writeLiveplayLock(pid, port) {
  try {
    fs.writeFileSync(
      liveplayLockPath(),
      JSON.stringify({ pid, port, startedAt: new Date().toISOString() }, null, 2),
    );
  } catch (e) {
    console.warn('[liveplay-server] could not write lock file:', e);
  }
}

function deleteLiveplayLock() {
  try { fs.unlinkSync(liveplayLockPath()); } catch {}
}

function isPidAlive(pid) {
  if (!Number.isInteger(pid) || pid <= 0) return false;
  try { process.kill(pid, 0); return true; }
  catch (e) { return e.code === 'EPERM'; /* process exists but we lack permission */ }
}

async function probeServerHealth(port, timeoutMs = 1000) {
  return new Promise((resolve) => {
    const req = require('http').get(
      { host: '127.0.0.1', port, path: '/api/health', timeout: timeoutMs },
      (res) => { resolve(res.statusCode === 200); res.resume(); },
    );
    req.on('timeout', () => { req.destroy(); resolve(false); });
    req.on('error',   () => resolve(false));
  });
}

function readLiveplayConfig() {
  try {
    const raw = fs.readFileSync(liveplayConfigPath(), 'utf-8');
    const parsed = JSON.parse(raw);
    return {
      mode:        parsed.mode === 'remote' ? 'remote' : 'local',
      remoteUrl:   typeof parsed.remoteUrl === 'string' ? parsed.remoteUrl : `http://127.0.0.1:${LIVEPLAY_DEFAULT_PORT}`,
      localPort:   Number.isInteger(parsed.localPort) ? parsed.localPort : LIVEPLAY_DEFAULT_PORT,
      // Off by default: the console window is how an operator sees the
      // server is still running after the app has gone. Silent mode moves
      // that to a tray icon the server owns.
      silentServer: parsed.silentServer === true,
    };
  } catch {
    return { mode: 'local', remoteUrl: `http://127.0.0.1:${LIVEPLAY_DEFAULT_PORT}`, localPort: LIVEPLAY_DEFAULT_PORT, silentServer: false };
  }
}

function writeLiveplayConfig(cfg) {
  try {
    fs.writeFileSync(liveplayConfigPath(), JSON.stringify(cfg, null, 2));
  } catch (e) {
    console.error('[liveplay-server] could not persist config:', e);
  }
}

// ---------------------------------------------------------------------------
// Recent-servers history (separate file so it survives config rewrites).
// Stored newest-first, capped, keyed by normalised URL.
// ---------------------------------------------------------------------------
const LIVEPLAY_RECENT_FILENAME = 'liveplay-recent-servers.json';
const LIVEPLAY_RECENT_MAX      = 8;

function liveplayRecentPath() {
  return path.join(app.getPath('userData'), LIVEPLAY_RECENT_FILENAME);
}

function readRecentServers() {
  try {
    const raw = fs.readFileSync(liveplayRecentPath(), 'utf-8');
    const arr = JSON.parse(raw);
    if (!Array.isArray(arr)) return [];
    return arr
      .filter(e => e && typeof e.url === 'string')
      .map(e => ({
        url:         e.url,
        name:        typeof e.name === 'string' ? e.name : '',
        host:        typeof e.host === 'string' ? e.host : '',
        port:        Number.isInteger(e.port) ? e.port : LIVEPLAY_DEFAULT_PORT,
        lastSeen:    Number.isInteger(e.lastSeen) ? e.lastSeen : 0,
      }))
      .slice(0, LIVEPLAY_RECENT_MAX);
  } catch {
    return [];
  }
}

function writeRecentServers(list) {
  try {
    fs.writeFileSync(liveplayRecentPath(), JSON.stringify(list, null, 2));
  } catch (e) {
    console.warn('[liveplay-discovery] could not persist recent servers:', e);
  }
}

function addRecentServer(entry) {
  if (!entry || typeof entry.url !== 'string' || !entry.url) return readRecentServers();
  // Strip trailing slash so the same server doesn't appear twice.
  const url = entry.url.replace(/\/+$/, '');
  const next = readRecentServers().filter(e => e.url !== url);
  next.unshift({
    url,
    name:     typeof entry.name === 'string' ? entry.name : '',
    host:     typeof entry.host === 'string' ? entry.host : '',
    port:     Number.isInteger(entry.port) ? entry.port : LIVEPLAY_DEFAULT_PORT,
    lastSeen: Date.now(),
  });
  const capped = next.slice(0, LIVEPLAY_RECENT_MAX);
  writeRecentServers(capped);
  return capped;
}

function removeRecentServer(url) {
  if (typeof url !== 'string') return readRecentServers();
  const clean = url.replace(/\/+$/, '');
  const next = readRecentServers().filter(e => e.url !== clean);
  writeRecentServers(next);
  return next;
}

// ---------------------------------------------------------------------------
// Recent-projects history — the last N .liveplay files this client opened.
// Stored newest-first, capped, keyed by the project file path. Per-client
// (lives in userData), so it follows the machine rather than the project.
// Surfaced as a File > Open Recent submenu and rebuilt whenever the list
// changes. Paths are server-filesystem paths (the same value openProject
// loads), so they resolve correctly as long as the same server is connected.
// ---------------------------------------------------------------------------
const LIVEPLAY_RECENT_PROJECTS_FILENAME = 'liveplay-recent-projects.json';
const LIVEPLAY_RECENT_PROJECTS_MAX      = 10;

function liveplayRecentProjectsPath() {
  return path.join(app.getPath('userData'), LIVEPLAY_RECENT_PROJECTS_FILENAME);
}

function readRecentProjects() {
  try {
    const raw = fs.readFileSync(liveplayRecentProjectsPath(), 'utf-8');
    const arr = JSON.parse(raw);
    if (!Array.isArray(arr)) return [];
    return arr
      .filter(e => e && typeof e.path === 'string' && e.path)
      .map(e => ({
        path:       e.path,
        name:       typeof e.name === 'string' ? e.name : '',
        folderPath: typeof e.folderPath === 'string' ? e.folderPath : '',
        lastOpened: Number.isInteger(e.lastOpened) ? e.lastOpened : 0,
      }))
      .slice(0, LIVEPLAY_RECENT_PROJECTS_MAX);
  } catch {
    return [];
  }
}

function writeRecentProjects(list) {
  try {
    fs.writeFileSync(liveplayRecentProjectsPath(), JSON.stringify(list, null, 2));
  } catch (e) {
    console.warn('[liveplay-projects] could not persist recent projects:', e);
  }
}

// ---------------------------------------------------------------------------
// Window bounds (P4, Electron half)
// ---------------------------------------------------------------------------
// Where each window was and how big, remembered in <userData> so it follows the
// machine rather than the project — the same tier and the same file shape as the
// recent-projects list above, and as the client-side pane layout in
// useWorkspaceLayout. All four windows were hardcoded before this: the main one
// opened 1400x900 in the middle of the primary display every launch, however the
// operator had arranged their desk the day before.
//
// THE TRAP HERE IS `useContentSize`. The main and mixer windows are created with
// it (it rides in MIN_WINDOW), so their constructor `width`/`height` mean the
// CONTENT box, while getBounds() reports the OUTER frame — about 15x64 larger on
// Windows. Save the outer size and feed it back as a content size and the window
// grows by one frame on every single launch. So each tracked window declares
// which it is, and its size is read from the matching getter.
//
// WHAT IS NOT RESTORED: which windows were open. Only their geometry. Recreating
// the detached mixer or cart at boot would need the renderer to learn about a
// window it did not ask for, and it would put a window on screen because of a
// choice made days ago — see the note in useWorkspaceLayout about why the
// detached flags are not persisted either.
const LIVEPLAY_WINDOW_BOUNDS_FILENAME = 'liveplay-window-bounds.json';
// A resize or move is a stream of events while the mouse is down; only the last
// one matters. Also flushed on close, where a pending timer would be lost.
const WINDOW_BOUNDS_DEBOUNCE_MS = 500;

function liveplayWindowBoundsPath() {
  return path.join(app.getPath('userData'), LIVEPLAY_WINDOW_BOUNDS_FILENAME);
}

function readWindowBoundsStore() {
  try {
    const raw = fs.readFileSync(liveplayWindowBoundsPath(), 'utf-8');
    const obj = JSON.parse(raw);
    return obj && typeof obj === 'object' && !Array.isArray(obj) ? obj : {};
  } catch {
    // No file yet, or unreadable. Every caller falls back to its own defaults.
    return {};
  }
}

function writeWindowBoundsStore(store) {
  try {
    fs.writeFileSync(liveplayWindowBoundsPath(), JSON.stringify(store, null, 2));
  } catch (e) {
    console.warn('[liveplay-window] could not persist window bounds:', e);
  }
}

const isPositiveInt = (v) => Number.isInteger(v) && v > 0;

/**
 * The saved geometry for `key`, merged over `defaults`, or `defaults` alone when
 * nothing usable is stored.
 *
 * Two things are checked rather than trusted, because both produce a window the
 * operator cannot get at:
 *   * the size must fit the display it will land on — a window saved on a 4K
 *     panel is taller than a laptop's work area, and a title bar dragged above
 *     the top of the screen cannot be grabbed back;
 *   * the position must still be ON a display. Unplug the second monitor and
 *     yesterday's x of 2800 is off the edge of the world. Dropping x/y lets
 *     Electron centre it, which is the same thing that happened before any of
 *     this existed.
 */
function savedWindowBounds(key, defaults) {
  const saved = readWindowBoundsStore()[key];
  if (!saved || typeof saved !== 'object') return { ...defaults };
  if (!isPositiveInt(saved.width) || !isPositiveInt(saved.height)) return { ...defaults };

  const out = { ...defaults, width: saved.width, height: saved.height };

  const hasPos = Number.isInteger(saved.x) && Number.isInteger(saved.y);
  // `getDisplayMatching` answers with the display that overlaps a rect most, and
  // falls back to the nearest one — so compare its work area against the rect to
  // find out whether there was any overlap at all.
  const area = hasPos
    ? screen.getDisplayMatching({ x: saved.x, y: saved.y, width: saved.width, height: saved.height }).workArea
    : screen.getPrimaryDisplay().workArea;

  // Never larger than the work area it is going onto. A content-sized window is
  // compared slightly generously (the frame is outside the number being stored),
  // which is fine: this is a sanity bound, not a layout.
  out.width  = Math.min(out.width,  area.width);
  out.height = Math.min(out.height, area.height);

  if (hasPos) {
    const onScreen =
      saved.x < area.x + area.width  && saved.x + saved.width  > area.x &&
      saved.y < area.y + area.height && saved.y + saved.height > area.y;
    if (onScreen) {
      out.x = saved.x;
      out.y = saved.y;
    }
  }
  if (saved.maximized === true) out.maximized = true;
  return out;
}

/**
 * What to store for a window, given what it currently reports and what was
 * stored for it last time. Pure, and separate from the window so it can be
 * tested — the maximized case below is subtle enough to be worth it.
 *
 * `prev` is the existing entry (or undefined), and `w` supplies the readings:
 *   { maximized, normalBounds, bounds, contentBounds, contentSize }
 */
function windowBoundsEntry(prev, w) {
  // While maximized, getBounds() is the maximized rectangle. Restoring THAT as a
  // normal size would give a window that fills the screen without being
  // maximized, so un-maximizing it would appear to do nothing.
  //
  // And the size is deliberately NOT taken from getNormalBounds() either, even
  // though that is the right rectangle: it is an OUTER rectangle, and a window
  // created with `useContentSize` would read it back as a content size and grow
  // by one window frame on every launch. There is no getNormalContentBounds().
  //
  // So maximizing records no new size at all — which is correct rather than a
  // workaround, because maximizing does not CHANGE the size the window would
  // return to. The previous entry's size is still the right answer; when there
  // is none (maximized before ever being resized) the size is simply omitted and
  // the caller's defaults apply underneath the maximize.
  if (w.maximized) {
    const out = { x: w.normalBounds.x, y: w.normalBounds.y, maximized: true };
    if (prev && Number.isInteger(prev.width) && Number.isInteger(prev.height)) {
      out.width = prev.width;
      out.height = prev.height;
    }
    return out;
  }

  const size = w.contentSize ? w.contentBounds : w.bounds;
  return {
    x: w.bounds.x, y: w.bounds.y,
    width: size.width, height: size.height,
    maximized: false,
  };
}

/**
 * Record this window's geometry as it changes.
 *
 * `contentSize` must match how the window was CREATED (see the note above): true
 * for a window built with `useContentSize`, so the number saved is the number the
 * constructor will read back, or the window gains a frame per launch.
 */
function trackWindowBounds(key, win, { contentSize = false } = {}) {
  let timer = null;

  const save = () => {
    timer = null;
    if (!win || win.isDestroyed()) return;
    // Minimized bounds are not the layout, and on some platforms not even
    // meaningful. Whatever was recorded a moment ago is still right.
    if (win.isMinimized()) return;

    const store = readWindowBoundsStore();
    store[key] = windowBoundsEntry(store[key], {
      maximized:     win.isMaximized(),
      normalBounds:  win.getNormalBounds(),
      bounds:        win.getBounds(),
      contentBounds: win.getContentBounds(),
      contentSize,
    });
    writeWindowBoundsStore(store);
  };

  const schedule = () => {
    if (timer) clearTimeout(timer);
    timer = setTimeout(save, WINDOW_BOUNDS_DEBOUNCE_MS);
  };

  win.on('resize', schedule);
  win.on('move', schedule);
  win.on('maximize', schedule);
  win.on('unmaximize', schedule);
  // Synchronously on the way out: 'closed' is too late to ask a window where it
  // was, and a debounce still pending here would be dropped with the window.
  win.on('close', () => {
    if (timer) { clearTimeout(timer); timer = null; }
    save();
  });
}

function addRecentProject(entry) {
  if (!entry || typeof entry.path !== 'string' || !entry.path) return readRecentProjects();
  // Normalise separators so the same file doesn't appear twice (e.g. when one
  // open used "/" and another "\"). Compare case-insensitively on Windows.
  const norm = (p) => {
    const s = p.replace(/[\\/]+/g, '/').replace(/\/+$/, '');
    return process.platform === 'win32' ? s.toLowerCase() : s;
  };
  const key = norm(entry.path);
  const next = readRecentProjects().filter(e => norm(e.path) !== key);
  next.unshift({
    path:       entry.path,
    name:       typeof entry.name === 'string' ? entry.name : '',
    folderPath: typeof entry.folderPath === 'string' ? entry.folderPath : '',
    lastOpened: Date.now(),
  });
  const capped = next.slice(0, LIVEPLAY_RECENT_PROJECTS_MAX);
  writeRecentProjects(capped);
  return capped;
}

function removeRecentProject(projectPath) {
  if (typeof projectPath !== 'string') return readRecentProjects();
  const norm = (p) => {
    const s = p.replace(/[\\/]+/g, '/').replace(/\/+$/, '');
    return process.platform === 'win32' ? s.toLowerCase() : s;
  };
  const key = norm(projectPath);
  const next = readRecentProjects().filter(e => norm(e.path) !== key);
  writeRecentProjects(next);
  return next;
}

function clearRecentProjects() {
  writeRecentProjects([]);
  return [];
}

// ---------------------------------------------------------------------------
// Best-effort Windows Firewall rules. Discovery relies on inbound UDP (the
// server receiving solicitations, the client receiving beacons/unicast
// replies). The NSIS installer adds proper program rules at install time
// (elevated); this runtime pass is a fallback for portable/dev runs. It is
// skipped unless we're elevated — netsh silently fails otherwise — and it
// only runs once per machine (a marker file guards repeat work).
// ---------------------------------------------------------------------------
function ensureFirewallRules() {
  if (process.platform !== 'win32') return;
  // Marker is versioned: bumping the suffix forces a one-time re-run so
  // existing installs that only got the old UDP-only pass pick up the new
  // server TCP rule.
  const markerPath = path.join(app.getPath('userData'), '.firewall-configured-v2');
  try { if (fs.existsSync(markerPath)) return; } catch {}

  const exe = process.execPath;
  const serverExe = resolveServerBinaryPath();
  // protocols per program:
  //  * UDP — discovery beacon/solicitation (both client and server listen).
  //  * TCP — the server's REST + WebSocket control port (4480). Without this,
  //    LAN discovery (UDP) still finds the server but every actual connection
  //    is silently dropped, so the client appears to connect yet nothing works.
  //    The client never accepts inbound TCP, so it only needs UDP.
  const rules = [
    ['LivePlay Client', exe,       ['UDP']],
    ['LivePlay Server', serverExe, ['UDP', 'TCP']],
  ];
  let attempted = false;
  for (const [name, program, protocols] of rules) {
    if (!program || !fs.existsSync(program)) continue;
    attempted = true;
    for (const proto of protocols) {
      // Program-scoped inbound allow — covers discovery / control on all ports.
      const cmd = `netsh advfirewall firewall add rule name="${name} (${proto}-In)" ` +
                  `dir=in action=allow protocol=${proto} program="${program}" enable=yes profile=any`;
      exec(cmd, (err) => {
        if (err) {
          // Almost always "requires elevation" — expected for non-admin runs.
          console.warn('[liveplay-firewall] could not add', proto, 'rule for', name,
                       '(needs admin / handled by installer):', err.message);
        } else {
          console.log('[liveplay-firewall] inbound', proto, 'rule ensured for', name);
        }
      });
    }
  }
  // Write the marker regardless so we don't re-spawn netsh every launch; the
  // installer is the authoritative path for users who declined elevation.
  if (attempted) {
    try { fs.writeFileSync(markerPath, new Date().toISOString()); } catch {}
  }
}

function resolveServerBinaryPath() {
  const exeName = process.platform === 'win32' ? 'liveplay-server.exe' : 'liveplay-server';

  if (app.isPackaged) {
    // Bundled via electron-builder extraResources → resourcesPath/server-bin/
    return path.join(process.resourcesPath, 'server-bin', exeName);
  }

  // Dev: client/electron/main.js → ../../server/build/{Release/}<exe>
  const repoServerDir = path.join(__dirname, '..', '..', 'server', 'build');
  const candidates = [
    path.join(repoServerDir, 'Release', exeName),  // MSBuild multi-config
    path.join(repoServerDir, exeName),              // Ninja single-config
  ];
  for (const p of candidates) {
    if (fs.existsSync(p)) return p;
  }
  return candidates[0]; // return first as a useful error path
}

// Passive: probe the lockfile and adopt the running server if there is
// one, but do NOT spawn a new one. Called at startup so users who quit
// the renderer while the detached server kept running rejoin
// transparently. Spawning is deferred to startLiveplayServer (which the
// renderer triggers only when the user picks Local mode).
async function tryReattachLiveplayServer() {
  if (liveplayServerProc || liveplayServerPid) return;
  const lock = readLiveplayLock();
  if (!lock) return;
  if (isPidAlive(lock.pid) && await probeServerHealth(lock.port)) {
    console.log('[liveplay-server] reattaching to pid', lock.pid, 'on port', lock.port);
    liveplayServerPid  = lock.pid;
    liveplayServerPort = lock.port;
    notifyServerStateChange();
    return;
  }
  // Stale — process gone or unhealthy. Clear the lock so future writes start clean.
  deleteLiveplayLock();
}

async function startLiveplayServer() {
  // Already managing one in this process — nothing to do.
  if (liveplayServerProc || liveplayServerPid) return;

  const cfg = readLiveplayConfig();
  if (cfg.mode !== 'local') return;

  // Try reattaching first in case another instance left a running server.
  await tryReattachLiveplayServer();
  if (liveplayServerProc || liveplayServerPid) return;

  const exePath = resolveServerBinaryPath();
  if (!fs.existsSync(exePath)) {
    console.error('[liveplay-server] binary not found at', exePath,
                  '— skipping spawn. Build the server (cmake --build server/build) or switch to Remote mode.');
    notifyServerStateChange();
    return;
  }

  // The server is launched as a normal application with its own console
  // window (Windows) or its own Terminal window (macOS) so the user can
  // see it in the taskbar/dock and stop it directly. We pass --pidfile so
  // the server writes its real PID to the lockfile — necessary on Windows
  // because `cmd /c start` returns the PID of cmd.exe, not the server.
  const lockPath = liveplayLockPath();
  // Pre-clear any stale lock so the freshly-started server's write is the
  // canonical one and our tryReattach race-condition window is minimal.
  deleteLiveplayLock();
  liveplayServerPort = cfg.localPort;

  const silent = cfg.silentServer === true || launchedSilent;
  console.log(`[liveplay-server] launching (${silent ? 'silent, tray icon' : 'visible console'})`, exePath, 'on port', cfg.localPort);
  const serverArgs = [
    '--port',    String(cfg.localPort),
    '--pidfile', lockPath,
  ];
  try {
    if (silent) {
      // No console or Terminal window: the server puts its own icon in the
      // system tray (menu bar on macOS), which stays for as long as it runs,
      // app or no app. windowsHide stops Windows flashing a console for it.
      liveplayServerProc = spawn(exePath, [...serverArgs, '--silent'], {
        cwd: path.dirname(exePath),
        stdio: 'ignore',
        detached: true,
        windowsHide: true,
      });
    } else if (process.platform === 'win32') {
      // `cmd /c start "" /D "<cwd>" "<exe>" <args>` opens a visible console
      // window in the taskbar. The empty-string title avoids the Windows quirk
      // where the first quoted arg to `start` is treated as the window title
      // instead of the executable. The server binary's own embedded icon and
      // the title it sets via SetConsoleTitle() then appear in the taskbar.
      liveplayServerProc = spawn(
        'cmd.exe',
        ['/c', 'start', '',
         '/D', path.dirname(exePath),
         exePath, ...serverArgs],
        {
          stdio: 'ignore',
          windowsHide: false,
          detached: true,
        },
      );
    } else if (process.platform === 'darwin') {
      // Open a Terminal.app window that runs the server. Quoting is fiddly
      // because the AppleScript is one string — escape the path manually.
      const shellQuote = (s) => `'${String(s).replace(/'/g, `'\\''`)}'`;
      const cmdLine = [exePath, ...serverArgs].map(shellQuote).join(' ');
      // Multi-statement AppleScript: open the window then activate so
      // Terminal comes to the foreground rather than opening silently
      // behind the LivePlay window.
      const appleScript = [
        'tell application "Terminal"',
        `  do script "${cmdLine.replace(/"/g, '\\"')}"`,
        '  activate',
        'end tell',
      ].join('\n');
      liveplayServerProc = spawn(
        'osascript',
        ['-e', appleScript],
        { stdio: 'ignore', detached: true },
      );
    } else {
      // Linux / other POSIX: spawn directly. If the user launched the
      // Electron app from a terminal, the server inherits that terminal
      // and is visible. If launched from a desktop launcher, there's no
      // console — best-effort.
      liveplayServerProc = spawn(exePath, serverArgs, {
        cwd: path.dirname(exePath),
        stdio: 'ignore',
        detached: true,
      });
    }
  } catch (e) {
    console.error('[liveplay-server] spawn failed:', e);
    liveplayServerProc = null;
    notifyServerStateChange();
    return;
  }

  // The spawn handle we hold is either cmd.exe (Windows), osascript (macOS),
  // or the server itself (Linux). We don't track its lifetime — the real
  // server's PID arrives via the pidfile within ~1 s. unref() so Electron
  // can quit independently.
  try { liveplayServerProc.unref(); } catch {}
  liveplayServerProc = null;   // discard the launcher handle

  // Poll the pidfile so the renderer (and onStateChange listeners) learn
  // the real PID. Don't await — this returns to the caller immediately;
  // ensureRunning() handles the "wait until healthy" gate.
  void pollPidfileForServerPid(lockPath);

  notifyServerStateChange();
}

// Watch the pidfile until the server writes it (or we time out). The
// server creates this file after binding, so its presence implies the
// server is ~ready.
async function pollPidfileForServerPid(lockPath) {
  const deadline = Date.now() + 8000;
  while (Date.now() < deadline) {
    const lock = readLiveplayLock();
    if (lock && Number.isInteger(lock.pid)) {
      liveplayServerPid  = lock.pid;
      liveplayServerPort = lock.port ?? liveplayServerPort;
      notifyServerStateChange();
      return;
    }
    await new Promise(r => setTimeout(r, 150));
  }
  console.warn('[liveplay-server] pidfile did not appear at', lockPath,
               '— server may have failed to launch (check the console window).');
}

// Stop the local server. Since the server now always runs as an external
// process (visible terminal, written PID via pidfile), we only have a PID
// to work with — there is no ChildProcess handle.
async function stopLiveplayServer() {
  let pid = liveplayServerPid;
  if (!pid) {
    // We lost track of it (e.g. the pidfile poll timed out), but the lockfile
    // may still name it. Only trust that PID if the server on its port
    // answers: a lock left by a crash or a reboot can name a PID the OS has
    // since handed to something else entirely.
    const lock = readLiveplayLock();
    if (lock && Number.isInteger(lock.pid) && isPidAlive(lock.pid) &&
        await probeServerHealth(lock.port)) {
      pid = lock.pid;
    }
  }
  if (!pid) {
    deleteLiveplayLock();
    notifyServerStateChange();
    return;
  }
  console.log('[liveplay-server] stopping pid', pid);
  try {
    if (process.platform === 'win32') {
      const taskkill = (args) => new Promise((resolve) => {
        exec(`taskkill /pid ${pid} ${args}`, (err) => resolve(!err));
      });
      // Ask first. A silent server's hidden tray window takes WM_CLOSE as
      // "stop", shuts down cleanly and takes its tray icon with it; a forced
      // kill would leave a dead icon behind until the mouse passes over it.
      // A console-hosted server refuses the polite request, so that costs
      // nothing. /T also closes the console window the server was hosted in.
      if (await taskkill('/T')) {
        const deadline = Date.now() + 3000;
        while (Date.now() < deadline && isPidAlive(pid)) {
          await new Promise(r => setTimeout(r, 100));
        }
      }
      if (isPidAlive(pid)) await taskkill('/T /F');
    } else {
      // 1. Send SIGTERM to process group and process
      try { process.kill(-pid, 'SIGTERM'); } catch {}
      try { process.kill(pid, 'SIGTERM'); } catch {}

      // 2. Wait up to 1.5 seconds for it to exit cleanly
      const deadline = Date.now() + 1500;
      while (Date.now() < deadline && isPidAlive(pid)) {
        await new Promise(r => setTimeout(r, 100));
      }

      // 3. If still alive, force kill with SIGKILL
      if (isPidAlive(pid)) {
        try { process.kill(-pid, 'SIGKILL'); } catch {}
        try { process.kill(pid, 'SIGKILL'); } catch {}
      }
    }
  } catch (e) {
    console.error('[liveplay-server] kill failed:', e);
  }
  deleteLiveplayLock();
  liveplayServerProc = null;
  liveplayServerPid  = null;
  notifyServerStateChange();
}

function liveplayServerStatus() {
  const pid = liveplayServerProc?.pid ?? liveplayServerPid;
  return {
    running: !!pid,
    pid,
    config:  readLiveplayConfig(),
  };
}

function notifyServerStateChange() {
  if (mainWindow && !mainWindow.isDestroyed()) {
    mainWindow.webContents.send('liveplay-server:state', liveplayServerStatus());
  }
}

// IPC: renderer reads/writes config and queries state.
ipcMain.handle('liveplay-server:get-config', () => ({ ...readLiveplayConfig(), launchedSilent }));

ipcMain.handle('liveplay-server:set-config', async (_e, incoming) => {
  const next = { ...readLiveplayConfig(), ...incoming };
  // Sanity: clamp port to a valid TCP range.
  if (!Number.isInteger(next.localPort) || next.localPort < 1 || next.localPort > 65535) {
    next.localPort = LIVEPLAY_DEFAULT_PORT;
  }
  if (next.mode !== 'local' && next.mode !== 'remote') next.mode = 'local';
  next.silentServer = next.silentServer === true;
  delete next.showConsole;   // the 2.5.1 pre-release spelling, inverted
  delete next.launchedSilent; // a fact about this launch, not a setting

  writeLiveplayConfig(next);

  // If switching to remote, stop any running local server.
  if (next.mode === 'remote' && (liveplayServerProc || liveplayServerPid)) await stopLiveplayServer();
  // Do NOT auto-start in local mode here — the caller (ensure-running) is
  // responsible for starting the server so it isn't spawned twice.

  return next;
});

ipcMain.handle('liveplay-server:get-status', () => liveplayServerStatus());

// Generic app lifecycle controls used by the connection-lost modal.
// `relaunch` re-spawns the renderer in a clean state (the detached
// liveplay-server keeps running and is reattached on the next launch).
// `exit` just quits without touching the server.
ipcMain.handle('app:relaunch', () => {
  app.relaunch();
  app.exit(0);
  return true;
});
ipcMain.handle('app:exit', () => {
  app.exit(0);
  return true;
});

// Two-step quit confirmation. The renderer drives the dialogs (unsaved
// changes → optionally shut the local audio server down); once the user
// has decided it calls `app:confirm-quit`. We stop the local server only
// when asked, flip quitConfirmed so the next `close` is allowed through,
// then quit for real.
ipcMain.handle('app:confirm-quit', async (_e, opts) => {
  if (opts && opts.stopServer) await stopLiveplayServer();
  quitConfirmed = true;
  app.quit();
  return true;
});

ipcMain.handle('liveplay-server:restart', async () => {
  await stopLiveplayServer();
  // Defer the restart to let the kill complete.
  setTimeout(() => startLiveplayServer(), 300);
  return true;
});

// Explicit shutdown — the server is now detached, so quitting the
// renderer no longer kills it. The user (or the about-to-quit prompt)
// invokes this when they really want it gone.
ipcMain.handle('liveplay-server:shutdown', async () => {
  await stopLiveplayServer();
  return true;
});

// Start the server (if not already running) and wait until /api/health
// answers. The welcome screen calls this when the user picks Local mode
// so the renderer doesn't try to connect before the server is bound.
// Returns { ok, port, error? } — never throws.
ipcMain.handle('liveplay-server:ensure-running', async () => {
  const cfg = readLiveplayConfig();
  if (cfg.mode !== 'local') {
    return { ok: false, port: cfg.localPort, error: 'config mode is not local' };
  }
  if (!liveplayServerProc && !liveplayServerPid) {
    try { await startLiveplayServer(); }
    catch (e) { return { ok: false, port: cfg.localPort, error: String(e) }; }
  }
  // Poll health. The detached console process needs a moment to bind.
  const deadline = Date.now() + 10000;
  while (Date.now() < deadline) {
    if (await probeServerHealth(cfg.localPort, 800)) {
      return { ok: true, port: cfg.localPort };
    }
    await new Promise(r => setTimeout(r, 200));
  }
  return { ok: false, port: cfg.localPort, error: 'server did not become healthy within 10s' };
});

// ===========================================================================
// LAN auto-discovery — listen for the C++ server's UDP beacon and surface
// every nearby server in the welcome screen's remote-mode picker.
// ---------------------------------------------------------------------------
// We bind a single dgram socket lazily on first discover-start request and
// keep it alive for the rest of the process — repeatedly binding/closing
// causes brief windows where beacons are missed. Last-seen entries are
// pruned after 12 s so a server that's been turned off disappears from
// the picker quickly enough to feel responsive.
// ===========================================================================
const dgram = require('dgram');
const DISCOVERY_PORT     = 4481;
const DISCOVERY_TIMEOUT  = 12000;
const DISCOVERY_GROUP    = '239.255.69.80';   // must match server DiscoveryConfig
const SOLICIT_PACKET     = Buffer.from(JSON.stringify({ type: 'liveplay-solicit' }), 'utf-8');
let discoverySocket  = null;
let discoveryStarted = false;
let solicitTimer     = null;
const discoveredServers = new Map(); // key: instanceId, value: {entry, lastSeen}

// Send an active "who's there" probe. Servers reply with a UNICAST beacon
// straight back to us — which traverses WiFi client-isolation and stateful
// firewalls that silently drop inbound broadcast/multicast. We fan it out to
// the limited broadcast and the multicast group; the unicast reply is what
// actually gets discovery working across awkward networks.
function sendSolicitation() {
  if (!discoverySocket) return;
  const targets = ['255.255.255.255', DISCOVERY_GROUP];
  for (const addr of targets) {
    try {
      discoverySocket.send(SOLICIT_PACKET, 0, SOLICIT_PACKET.length, DISCOVERY_PORT, addr);
    } catch {}
  }
}

function startDiscoveryListener() {
  if (discoveryStarted) return;
  discoveryStarted = true;
  try {
    discoverySocket = dgram.createSocket({ type: 'udp4', reuseAddr: true });
  } catch (e) {
    console.warn('[liveplay-discovery] createSocket failed:', e);
    discoveryStarted = false;
    return;
  }
  discoverySocket.on('error', (err) => {
    console.warn('[liveplay-discovery] socket error:', err.message);
  });
  discoverySocket.on('message', (buf, rinfo) => {
    let msg;
    try { msg = JSON.parse(buf.toString('utf-8')); } catch { return; }
    // Ignore our own solicitations (and anything that isn't a beacon).
    if (!msg || msg.type !== 'liveplay-beacon') return;
    const id = String(msg.instanceId || `${rinfo.address}:${msg.port}`);
    const entry = {
      instanceId:     id,
      name:           String(msg.name || 'liveplay'),
      host:           rinfo.address,                       // LAN IP the packet came from
      port:           Number.isInteger(msg.port) ? msg.port : 4480,
      version:        String(msg.version || ''),
      projectName:    String(msg.projectName || ''),
      hasOpenProject: !!msg.hasOpenProject,
      itemCount:      Number.isInteger(msg.itemCount) ? msg.itemCount : 0,
      url:            `http://${rinfo.address}:${Number.isInteger(msg.port) ? msg.port : 4480}`,
    };
    discoveredServers.set(id, { entry, lastSeen: Date.now() });
    broadcastDiscovered();
  });
  try {
    // Bind on 0.0.0.0:DISCOVERY_PORT to receive broadcasts. reuseAddr lets
    // multiple LivePlay clients on the same machine coexist.
    discoverySocket.bind(DISCOVERY_PORT, () => {
      try { discoverySocket.setBroadcast(true); } catch {}
      // Join the multicast group on all interfaces (no iface arg = default
      // chosen by the OS; we also try each known interface below). Some
      // networks pass multicast where broadcast is filtered.
      try { discoverySocket.addMembership(DISCOVERY_GROUP); } catch {}
      try {
        const os = require('os');
        const ifaces = os.networkInterfaces();
        for (const name of Object.keys(ifaces)) {
          for (const ni of ifaces[name] || []) {
            if (ni.family === 'IPv4' && !ni.internal) {
              try { discoverySocket.addMembership(DISCOVERY_GROUP, ni.address); } catch {}
            }
          }
        }
      } catch {}
      console.log('[liveplay-discovery] listening on UDP/' + DISCOVERY_PORT);
      // Kick off discovery immediately with a short burst so the picker
      // populates within a second instead of waiting for a passive beacon.
      sendSolicitation();
      setTimeout(sendSolicitation, 250);
      setTimeout(sendSolicitation, 750);
    });
  } catch (e) {
    console.warn('[liveplay-discovery] bind failed:', e);
  }

  // Keep soliciting on an interval — cheap, and means servers that come up
  // later (or clients that just opened the picker) are found promptly even
  // if their broadcasts aren't reaching us.
  solicitTimer = setInterval(sendSolicitation, 3000);
  solicitTimer.unref?.();

  // Periodically prune stale entries.
  setInterval(() => {
    const now = Date.now();
    let changed = false;
    for (const [id, v] of discoveredServers) {
      if (now - v.lastSeen > DISCOVERY_TIMEOUT) {
        discoveredServers.delete(id);
        changed = true;
      }
    }
    if (changed) broadcastDiscovered();
  }, 4000).unref();
}

function broadcastDiscovered() {
  if (!mainWindow || mainWindow.isDestroyed()) return;
  const list = Array.from(discoveredServers.values()).map(v => v.entry);
  mainWindow.webContents.send('liveplay-discovery:servers', list);
}

ipcMain.handle('liveplay-discovery:start', () => {
  startDiscoveryListener();
  sendSolicitation();             // fresh probe whenever the picker opens
  broadcastDiscovered();          // immediate refresh for the new subscriber
  return true;
});

ipcMain.handle('liveplay-discovery:list', () => {
  return Array.from(discoveredServers.values()).map(v => v.entry);
});

// Fire an on-demand solicitation (e.g. user hit a "rescan" button).
ipcMain.handle('liveplay-discovery:solicit', () => {
  startDiscoveryListener();
  sendSolicitation();
  return true;
});

// Recent-servers history — the robust fallback when discovery can't reach a
// server (different subnet, VPN, locked-down WiFi). The renderer records a
// server here whenever it successfully connects, and reads the list to offer
// one-tap reconnect + auto-reconnect on launch.
ipcMain.handle('liveplay-discovery:recent-list', () => readRecentServers());

ipcMain.handle('liveplay-discovery:recent-add', (_e, entry) => {
  return addRecentServer(entry);
});

ipcMain.handle('liveplay-discovery:recent-remove', (_e, url) => {
  return removeRecentServer(url);
});

// Recent-projects history — last N .liveplay files opened on this client.
// Every mutation rebuilds the menu so the File > Open Recent submenu stays
// in sync without the renderer having to poke the menu directly.
ipcMain.handle('liveplay-projects:recent-list', () => readRecentProjects());

ipcMain.handle('liveplay-projects:recent-add', (_e, entry) => {
  const list = addRecentProject(entry);
  createMenu(currentLocale, isDevMode);
  return list;
});

ipcMain.handle('liveplay-projects:recent-remove', (_e, projectPath) => {
  const list = removeRecentProject(projectPath);
  createMenu(currentLocale, isDevMode);
  return list;
});

ipcMain.handle('liveplay-projects:recent-clear', () => {
  const list = clearRecentProjects();
  createMenu(currentLocale, isDevMode);
  return list;
});

// Setup bundled ffmpeg - always use the bundled version to avoid
// issues on OS's with strict security requirements
async function checkAndSetupFfmpeg() {
  try {
    const ffmpegInstaller = require('@ffmpeg-installer/ffmpeg');
    ffmpegPath = ffmpegInstaller.path;
    
    // In packaged app, the path may be inside app.asar - resolve it
    if (ffmpegPath.includes('app.asar')) {
      ffmpegPath = ffmpegPath.replace('app.asar', 'app.asar.unpacked');
    }
    
    // Verify bundled version works
    await execPromise(`"${ffmpegPath}" -version`);
    ffmpegAvailable = true;
    console.log('Using bundled ffmpeg:', ffmpegPath);
    
    // Set ffprobe path from @ffprobe-installer/ffprobe
    try {
      const ffprobeInstaller = require('@ffprobe-installer/ffprobe');
      let ffprobePath = ffprobeInstaller.path;
      if (ffprobePath.includes('app.asar')) {
        ffprobePath = ffprobePath.replace('app.asar', 'app.asar.unpacked');
      }
      ffmpeg.setFfprobePath(ffprobePath);
      console.log('Using bundled ffprobe:', ffprobePath);
    } catch (e) {
      // Fallback: try ffprobe next to ffmpeg
      const ffprobeFileName = process.platform === 'win32' ? 'ffprobe.exe' : 'ffprobe';
      const ffprobePath = path.join(path.dirname(ffmpegPath), ffprobeFileName);
      if (fs.existsSync(ffprobePath)) {
        ffmpeg.setFfprobePath(ffprobePath);
        console.log('Using ffprobe from ffmpeg directory:', ffprobePath);
      } else {
        // Fallback: try system-wide ffprobe from PATH
        try {
          const whichProbeCmd = process.platform === 'win32' ? 'where ffprobe' : 'which ffprobe';
          const probeResult = await execPromise(whichProbeCmd);
          const systemProbePath = probeResult.stdout.trim().split('\n')[0].trim();
          ffmpeg.setFfprobePath(systemProbePath);
          console.log('Using system ffprobe:', systemProbePath);
        } catch (probeErr) {
          console.warn('ffprobe not found, some features may be limited');
        }
      }
    }
    
    return true;
  } catch (error) {
    console.error('Failed to setup bundled ffmpeg:', error);
    
    // Fallback: try system-wide ffmpeg from PATH
    try {
      const whichCmd = process.platform === 'win32' ? 'where ffmpeg' : 'which ffmpeg';
      const { stdout } = await execPromise(whichCmd);
      const systemFfmpegPath = stdout.trim().split('\n')[0].trim();
      
      // Verify the system binary works
      await execPromise(`"${systemFfmpegPath}" -version`);
      ffmpegPath = systemFfmpegPath;
      ffmpegAvailable = true;
      console.log('Using system ffmpeg:', ffmpegPath);
      
      // Try to find system ffprobe too
      try {
        const whichProbeCmd = process.platform === 'win32' ? 'where ffprobe' : 'which ffprobe';
        const probeResult = await execPromise(whichProbeCmd);
        const systemFfprobePath = probeResult.stdout.trim().split('\n')[0].trim();
        ffmpeg.setFfprobePath(systemFfprobePath);
        console.log('Using system ffprobe:', systemFfprobePath);
      } catch (probeErr) {
        console.warn('System ffprobe not found, some features may be limited');
      }
      
      return true;
    } catch (systemError) {
      console.error('System ffmpeg not found either:', systemError.message);
      ffmpegAvailable = false;
      return false;
    }
  }
}

// ===========================================================================
// yt-dlp + deno (JS runtime) management
// ---------------------------------------------------------------------------
// YouTube changes its player/signature scheme frequently, so the bundled
// yt-dlp binary goes stale fast. Rather than the old "re-download if the file
// is >7 days old" heuristic (which both updated needlessly and could keep a
// stale binary for up to a week), we compare the installed --version against
// the latest GitHub release on every launch and update whenever they differ.
//
// Modern yt-dlp also requires a JavaScript runtime to solve YouTube's nsig
// challenge ("No supported JavaScript runtime could be found" warning).
// Electron's own Node (18 in Electron 28) is too old for yt-dlp's JS provider,
// so we download deno — yt-dlp's recommended runtime — alongside yt-dlp and
// pass it via --js-runtimes. Both downloads are best-effort and degrade
// gracefully: network failures fall back to whatever binary is already on disk.
// ===========================================================================
let ytDlpPath;
let ytDlpReady = false;
let denoPath = null;
let denoReady = false;

// Fetch the latest release tag for a GitHub repo (e.g. "yt-dlp/yt-dlp").
// Best-effort: rejects on network error / non-200 so callers can fall back.
function getLatestReleaseTag(repo) {
  return new Promise((resolve, reject) => {
    const req = https.request({
      hostname: 'api.github.com',
      path: `/repos/${repo}/releases/latest`,
      method: 'GET',
      headers: {
        'User-Agent': 'LivePlay',
        'Accept': 'application/vnd.github+json'
      },
      timeout: 10000
    }, (res) => {
      let data = '';
      res.on('data', (chunk) => { data += chunk; });
      res.on('end', () => {
        if (res.statusCode !== 200) {
          reject(new Error(`GitHub API returned ${res.statusCode} for ${repo}`));
          return;
        }
        try {
          resolve(JSON.parse(data).tag_name);
        } catch (e) {
          reject(e);
        }
      });
    });
    req.on('error', reject);
    req.on('timeout', () => { req.destroy(new Error('GitHub API request timed out')); });
    req.end();
  });
}

// Run `<binary> --version` and return the trimmed stdout, or null on failure.
async function getBinaryVersion(binaryPath) {
  try {
    const { stdout } = await execPromise(`"${binaryPath}" --version`);
    return stdout.trim();
  } catch (e) {
    return null;
  }
}

// Map current platform to yt-dlp standalone release asset name.
// Note: yt-dlp-wrap's default downloadFromGithub() fetches "yt-dlp" on macOS/Linux,
// which is a python zipapp requiring Python >= 3.10 to be installed on the host.
// On macOS (and Linux), downloading the standalone binary (yt-dlp_macos / yt-dlp_linux)
// avoids any dependency on system python versions.
function getYtDlpAssetName() {
  switch (process.platform) {
    case 'win32':
      return 'yt-dlp.exe';
    case 'darwin':
      return 'yt-dlp_macos';
    case 'linux':
      return process.arch === 'arm64' ? 'yt-dlp_linux_aarch64' : 'yt-dlp_linux';
    default:
      return 'yt-dlp';
  }
}

async function initializeYtDlp() {
  try {
    // Set up download directory in user data folder
    const binDir = path.join(app.getPath('userData'), 'bin');
    if (!fs.existsSync(binDir)) {
      fs.mkdirSync(binDir, { recursive: true });
    }

    const exeName = process.platform === 'win32' ? 'yt-dlp.exe' : 'yt-dlp';
    const binaryPath = path.join(binDir, exeName);
    const exists = fs.existsSync(binaryPath);

    console.log('Initializing yt-dlp...');
    console.log('Binary path:', binaryPath);

    // Determine the latest available version (best-effort; non-fatal on failure).
    let latestVersion = null;
    try {
      latestVersion = await getLatestReleaseTag('yt-dlp/yt-dlp');
    } catch (e) {
      console.warn('Could not check latest yt-dlp version:', e.message);
    }

    // Determine the currently installed version (yt-dlp prints e.g. "2026.03.17").
    const installedVersion = exists ? await getBinaryVersion(binaryPath) : null;

    // Update when the binary is missing, or we know the latest and it differs
    // from what's installed (including when we can't read the installed version).
    let needsDownload = !exists;
    if (!needsDownload && latestVersion) {
      if (installedVersion !== latestVersion) {
        console.log(`yt-dlp update: installed=${installedVersion || 'unknown'} latest=${latestVersion}`);
        needsDownload = true;
      } else {
        console.log(`yt-dlp is up to date (${installedVersion})`);
      }
    }

    if (needsDownload) {
      console.log('Downloading latest yt-dlp binary...');
      // Back up old binary in case download fails
      const backupPath = binaryPath + '.bak';
      try {
        if (exists) {
          fs.copyFileSync(binaryPath, backupPath);
          fs.unlinkSync(binaryPath);
        }
        const assetName = getYtDlpAssetName();
        const url = latestVersion
          ? `https://github.com/yt-dlp/yt-dlp/releases/download/${latestVersion}/${assetName}`
          : `https://github.com/yt-dlp/yt-dlp/releases/latest/download/${assetName}`;
        console.log(`Downloading yt-dlp binary from ${url}...`);
        try {
          await YTDlpWrap.downloadFile(url, binaryPath);
        } catch (assetErr) {
          console.warn(`Direct asset download failed for ${assetName}, falling back to yt-dlp-wrap:`, assetErr.message);
          await YTDlpWrap.downloadFromGithub(binaryPath, latestVersion || undefined);
        }
        if (process.platform !== 'win32' && fs.existsSync(binaryPath)) {
          fs.chmodSync(binaryPath, 0o755);
        }
        // Clean up backup on success
        if (fs.existsSync(backupPath)) {
          fs.unlinkSync(backupPath);
        }
      } catch (downloadError) {
        console.error('Failed to download yt-dlp:', downloadError);
        // Restore backup if download failed
        if (fs.existsSync(backupPath)) {
          fs.copyFileSync(backupPath, binaryPath);
          fs.unlinkSync(backupPath);
          console.log('Restored previous yt-dlp binary as fallback');
        } else if (!fs.existsSync(binaryPath)) {
          throw downloadError;
        }
      }
    }

    // Verify the binary exists and is usable
    if (fs.existsSync(binaryPath)) {
      ytDlpPath = binaryPath;
      ytDlpReady = true;
      console.log('yt-dlp version:', (await getBinaryVersion(binaryPath)) || 'unknown');
      return true;
    } else {
      throw new Error('yt-dlp binary not found after initialization');
    }
  } catch (error) {
    console.error('Failed to initialize yt-dlp:', error);
    ytDlpReady = false;
    return false;
  }
}

// Map the current platform/arch to deno's release asset filename.
function getDenoAssetName() {
  const arch = process.arch === 'arm64' ? 'aarch64' : 'x86_64';
  switch (process.platform) {
    case 'win32': return `deno-${arch}-pc-windows-msvc.zip`;
    case 'darwin': return `deno-${arch}-apple-darwin.zip`;
    case 'linux': return `deno-${arch}-unknown-linux-gnu.zip`;
    default: return null;
  }
}

// Download/update deno, the JS runtime yt-dlp uses to solve YouTube's nsig
// challenge. Same version-comparison + backup/restore strategy as yt-dlp.
async function initializeDeno() {
  try {
    const assetName = getDenoAssetName();
    if (!assetName) {
      console.warn(`deno is not available for ${process.platform}/${process.arch}; YouTube extraction may be degraded.`);
      return false;
    }

    const binDir = path.join(app.getPath('userData'), 'bin');
    if (!fs.existsSync(binDir)) {
      fs.mkdirSync(binDir, { recursive: true });
    }

    const exeName = process.platform === 'win32' ? 'deno.exe' : 'deno';
    const binaryPath = path.join(binDir, exeName);
    const exists = fs.existsSync(binaryPath);

    console.log('Initializing deno (JS runtime for yt-dlp)...');
    console.log('Binary path:', binaryPath);

    // Latest deno tag looks like "v2.8.1"; normalise to "2.8.1" for comparison.
    let latestTag = null;
    try {
      latestTag = await getLatestReleaseTag('denoland/deno');
    } catch (e) {
      console.warn('Could not check latest deno version:', e.message);
    }
    const latestVersion = latestTag ? latestTag.replace(/^v/, '') : null;

    // `deno --version` prints e.g. "deno 2.8.1 (stable, release, ...)".
    let installedVersion = null;
    if (exists) {
      const out = await getBinaryVersion(binaryPath);
      const m = out && out.match(/deno\s+(\d+\.\d+\.\d+)/i);
      installedVersion = m ? m[1] : null;
    }

    let needsDownload = !exists;
    if (!needsDownload && latestVersion && installedVersion !== latestVersion) {
      console.log(`deno update: installed=${installedVersion || 'unknown'} latest=${latestVersion}`);
      needsDownload = true;
    } else if (!needsDownload) {
      console.log(`deno is up to date (${installedVersion || 'unknown'})`);
    }

    if (needsDownload) {
      const url = latestTag
        ? `https://github.com/denoland/deno/releases/download/${latestTag}/${assetName}`
        : `https://github.com/denoland/deno/releases/latest/download/${assetName}`;
      const zipPath = path.join(binDir, assetName);
      const backupPath = binaryPath + '.bak';
      try {
        if (exists) {
          fs.copyFileSync(binaryPath, backupPath);
          fs.unlinkSync(binaryPath);
        }
        console.log('Downloading deno from', url);
        await YTDlpWrap.downloadFile(url, zipPath);
        const extractZip = require('extract-zip');
        await extractZip(zipPath, { dir: binDir });
        if (fs.existsSync(zipPath)) fs.unlinkSync(zipPath);
        // The zip should preserve the exec bit, but ensure it on unix.
        if (process.platform !== 'win32' && fs.existsSync(binaryPath)) {
          fs.chmodSync(binaryPath, 0o755);
        }
        if (fs.existsSync(backupPath)) fs.unlinkSync(backupPath);
      } catch (err) {
        console.error('Failed to download/extract deno:', err);
        if (fs.existsSync(backupPath)) {
          fs.copyFileSync(backupPath, binaryPath);
          fs.unlinkSync(backupPath);
          console.log('Restored previous deno binary as fallback');
        }
      }
    }

    if (fs.existsSync(binaryPath)) {
      denoPath = binaryPath;
      denoReady = true;
      const out = await getBinaryVersion(binaryPath);
      console.log('deno version:', out ? out.split('\n')[0].trim() : 'unknown');
      return true;
    }

    console.warn('deno binary not available; YouTube extraction will run without a JS runtime.');
    return false;
  } catch (error) {
    console.error('Failed to initialize deno:', error);
    denoReady = false;
    return false;
  }
}

// Start initialization immediately (concurrently; both are independent).
initializeYtDlp();
initializeDeno();

let mainWindow = null;
// Set once the renderer-driven quit confirmation has resolved, so the
// next main-window `close` is allowed through instead of being vetoed
// to re-show the dialogs. See the `close` handler in createWindow().
let quitConfirmed = false;
let apiServer = null;
let currentProject = null;
let currentProjectData = null; // Full project data synced from renderer for HTTP API
const pendingApiRequests = new Map(); // requestId → { resolve } for PATCH round-trips
let fileToOpen = null; // Store file path if app is opened with a file
let stateViewerWindow = null; // Debug state viewer window
let cartPlayerWindow = null;  // Detached cart player window
let mixerWindow = null;       // Detached mixer window

// Minimum size for windows that host the full editing surface. Applied to the
// main window and the detached mixer, both of which render the mixer's channel
// view. Not applied to the cart player: that is a pad grid, and shrinking it to
// a corner of a screen is a legitimate way to use it.
//
// useContentSize matters here. Without it these numbers describe the outer
// window, frame included, so a 1280x768 minimum left the page itself at
// 1266x706 on Windows — the borders and title bar eat 14 and 62 — and every
// CSS breakpoint and vh unit in the layout is measured against the page, not
// the frame. With it, the numbers mean what the stylesheets assume.
//
// Sized so the app FITS a 1366x768 laptop panel, which is the smallest screen
// it is expected to run on.
//
// These are CONTENT dimensions (useContentSize), which is the whole reason the
// numbers look odd: on Windows the frame and title bar add roughly 15x64, so
// 1350x690 content is about 1365x754 outer, and a 768px-tall screen leaves
// roughly 720px once the taskbar is out. Setting the minimum to 1366x768
// content — the obvious-looking choice, and what was briefly here — yields a
// ~1381x832 window and locks that laptop out entirely. So does the 1280x720
// this replaced, which measured ~1295x784.
//
// 690px of content height is below the channel view's comfortable three-column
// height, and that is fine rather than an oversight: the work area scrolls and
// the dynamics panel scrolls inside it, so a short window is cramped instead of
// broken. The floor exists to keep the layout usable, not to guarantee every
// panel its full height.
const MIN_WINDOW = { minWidth: 1350, minHeight: 690, useContentSize: true };

// Flatten all audio items from a nested project items array
function flattenAudioItems(items) {
  const result = [];
  for (const item of items) {
    if (item.type === 'audio') result.push(item);
    if (item.type === 'group' && item.children) result.push(...flattenAudioItems(item.children));
  }
  return result;
}

// Find any item by UUID in a nested items array
function findProjectItemByUuid(items, uuid) {
  for (const item of items) {
    if (item.uuid === uuid) return item;
    if (item.type === 'group' && item.children) {
      const found = findProjectItemByUuid(item.children, uuid);
      if (found) return found;
    }
  }
  return null;
}

// Check if --dev flag is present in command line arguments
const isDevMode = process.argv.includes('--dev') || !app.isPackaged;

// API Server Setup
function startAPIServer(port = 8080, maxAttempts = 10) {
  const apiApp = express();
  apiApp.use(express.json());

  // ── Cue endpoints ──────────────────────────────────────────────────────────

  // List all cue items (audio items from playlist, flattened)
  apiApp.get('/api/cues', (req, res) => {
    if (!currentProjectData) {
      return res.status(404).json({ success: false, message: 'No project loaded' });
    }
    const cues = flattenAudioItems(currentProjectData.items || []);
    res.json({ success: true, cues });
  });

  // Get a cue by UUID
  apiApp.get('/api/cues/:id', (req, res) => {
    if (!currentProjectData) {
      return res.status(404).json({ success: false, message: 'No project loaded' });
    }
    const item = findProjectItemByUuid(currentProjectData.items || [], req.params.id);
    if (!item || item.type !== 'audio') {
      return res.status(404).json({ success: false, message: 'Cue not found' });
    }
    res.json({ success: true, cue: item });
  });

  // Update a cue by UUID (partial update — only provided fields are changed)
  apiApp.patch('/api/cues/:id', async (req, res) => {
    if (!mainWindow) return res.status(500).json({ success: false, message: 'Window not available' });
    if (!currentProjectData) return res.status(404).json({ success: false, message: 'No project loaded' });
    const item = findProjectItemByUuid(currentProjectData.items || [], req.params.id);
    if (!item || item.type !== 'audio') {
      return res.status(404).json({ success: false, message: 'Cue not found' });
    }
    const requestId = `${Date.now()}-${Math.random().toString(36).slice(2)}`;
    const promise = new Promise((resolve) => {
      const timer = setTimeout(() => {
        pendingApiRequests.delete(requestId);
        resolve({ success: false, message: 'Request timeout' });
      }, 5000);
      pendingApiRequests.set(requestId, { resolve: (r) => { clearTimeout(timer); resolve(r); } });
    });
    mainWindow.webContents.send('api-update-item', { requestId, id: req.params.id, updates: req.body });
    const result = await promise;
    res.status(result.success ? 200 : 500).json(result);
  });

  // ── Cart endpoints ─────────────────────────────────────────────────────────

  // List all cart slots with their audio item details
  apiApp.get('/api/carts', (req, res) => {
    if (!currentProjectData) {
      return res.status(404).json({ success: false, message: 'No project loaded' });
    }
    const cartOnlyItems = currentProjectData.cartOnlyItems || [];
    const carts = (currentProjectData.cartItems || []).map(ci => {
      const item = cartOnlyItems.find(i => i.uuid === ci.itemUuid)
        || findProjectItemByUuid(currentProjectData.items || [], ci.itemUuid)
        || null;
      return { slot: ci.slot, itemUuid: ci.itemUuid, item };
    });
    res.json({ success: true, carts });
  });

  // Get a cart slot by slot number (0-15)
  apiApp.get('/api/carts/:slot', (req, res) => {
    if (!currentProjectData) {
      return res.status(404).json({ success: false, message: 'No project loaded' });
    }
    const slot = parseInt(req.params.slot, 10);
    if (isNaN(slot) || slot < 0 || slot > 15) {
      return res.status(400).json({ success: false, message: 'Invalid slot number — must be 0-15' });
    }
    const ci = (currentProjectData.cartItems || []).find(c => c.slot === slot);
    if (!ci) {
      return res.status(404).json({ success: false, message: 'Cart slot is empty' });
    }
    const cartOnlyItems = currentProjectData.cartOnlyItems || [];
    const item = cartOnlyItems.find(i => i.uuid === ci.itemUuid)
      || findProjectItemByUuid(currentProjectData.items || [], ci.itemUuid)
      || null;
    res.json({ success: true, cart: { slot: ci.slot, itemUuid: ci.itemUuid, item } });
  });

  // Update a cart slot's audio item by slot number (partial update)
  apiApp.patch('/api/carts/:slot', async (req, res) => {
    if (!mainWindow) return res.status(500).json({ success: false, message: 'Window not available' });
    if (!currentProjectData) return res.status(404).json({ success: false, message: 'No project loaded' });
    const slot = parseInt(req.params.slot, 10);
    if (isNaN(slot) || slot < 0 || slot > 15) {
      return res.status(400).json({ success: false, message: 'Invalid slot number — must be 0-15' });
    }
    const ci = (currentProjectData.cartItems || []).find(c => c.slot === slot);
    if (!ci) return res.status(404).json({ success: false, message: 'Cart slot is empty' });
    const requestId = `${Date.now()}-${Math.random().toString(36).slice(2)}`;
    const promise = new Promise((resolve) => {
      const timer = setTimeout(() => {
        pendingApiRequests.delete(requestId);
        resolve({ success: false, message: 'Request timeout' });
      }, 5000);
      pendingApiRequests.set(requestId, { resolve: (r) => { clearTimeout(timer); resolve(r); } });
    });
    mainWindow.webContents.send('api-update-cart-item', { requestId, slot, updates: req.body });
    const result = await promise;
    res.status(result.success ? 200 : 500).json(result);
  });

  // ── Trigger endpoints ──────────────────────────────────────────────────────

  // Trigger cue by UUID
  apiApp.get('/api/trigger/uuid/:uuid', (req, res) => {
    const { uuid } = req.params;
    if (mainWindow) {
      mainWindow.webContents.send('trigger-item', { type: 'uuid', value: uuid });
      res.json({ success: true, message: `Triggered item ${uuid}` });
    } else {
      res.status(500).json({ success: false, message: 'Window not available' });
    }
  });

  // Trigger cue by index (comma-separated, e.g. "0" or "1,2")
  apiApp.get('/api/trigger/index/:index', (req, res) => {
    const { index } = req.params;
    if (mainWindow) {
      const indexArray = index.split(',').map(i => parseInt(i.trim()));
      mainWindow.webContents.send('trigger-item', { type: 'index', value: indexArray });
      res.json({ success: true, message: `Triggered item at index ${index}` });
    } else {
      res.status(500).json({ success: false, message: 'Window not available' });
    }
  });

  // Trigger a cart slot by slot number (0-15)
  apiApp.get('/api/trigger/cart/slot/:slot', (req, res) => {
    const slot = parseInt(req.params.slot, 10);
    if (isNaN(slot) || slot < 0 || slot > 15) {
      return res.status(400).json({ success: false, message: 'Invalid slot number — must be 0-15' });
    }
    if (mainWindow) {
      mainWindow.webContents.send('trigger-cart-slot', { slot });
      res.json({ success: true, message: `Triggered cart slot ${slot + 1}` });
    } else {
      res.status(500).json({ success: false, message: 'Window not available' });
    }
  });

  // ── Stop endpoints ─────────────────────────────────────────────────────────

  // Stop a cue by UUID
  apiApp.get('/api/stop/uuid/:uuid', (req, res) => {
    const { uuid } = req.params;
    if (mainWindow) {
      mainWindow.webContents.send('stop-item', { type: 'uuid', value: uuid });
      res.json({ success: true, message: `Stopped item ${uuid}` });
    } else {
      res.status(500).json({ success: false, message: 'Window not available' });
    }
  });

  // Stop all cues
  apiApp.get('/api/stop/all', (req, res) => {
    if (mainWindow) {
      mainWindow.webContents.send('stop-all-cues', {});
      res.json({ success: true, message: 'All cues stopped' });
    } else {
      res.status(500).json({ success: false, message: 'Window not available' });
    }
  });

  // ── Project info ───────────────────────────────────────────────────────────

  // Get current project info
  apiApp.get('/api/project/info', (req, res) => {
    if (currentProjectData) {
      res.json({ success: true, project: currentProjectData });
    } else if (currentProject) {
      res.json({ success: true, project: { path: currentProject } });
    } else {
      res.status(404).json({ success: false, message: 'No project loaded' });
    }
  });

  // Try to start server, incrementing port if already in use
  const tryListen = (currentPort, attemptsLeft) => {
    const server = apiApp.listen(currentPort)
      .on('listening', () => {
        apiServer = server;
        console.log(`LivePlay API Server running on http://localhost:${currentPort}`);
      })
      .on('error', (err) => {
        if (err.code === 'EADDRINUSE' && attemptsLeft > 0) {
          console.log(`Port ${currentPort} is in use, trying ${currentPort + 1}...`);
          tryListen(currentPort + 1, attemptsLeft - 1);
        } else if (err.code === 'EADDRINUSE') {
          console.error(`Failed to start API server after ${maxAttempts} attempts. Ports ${port}-${currentPort} are all in use.`);
        } else {
          console.error('Failed to start API server:', err);
        }
      });
  };

  tryListen(port, maxAttempts - 1);
}

// Configure auto-updater
autoUpdater.autoDownload = false; // Don't auto-download, ask user first
autoUpdater.autoInstallOnAppQuit = true;

// Configure update feed URL to point to GitHub releases
autoUpdater.setFeedURL({
  provider: 'github',
  owner: 'tdoukinitsas',
  repo: 'liveplay',
  private: false
});

console.log('Auto-updater configured for:', autoUpdater.getFeedURL());

// Auto-updater event handlers
autoUpdater.on('checking-for-update', () => {
  console.log('Checking for updates...');
});

autoUpdater.on('update-available', (info) => {
  console.log('Update available:', info.version);
  if (mainWindow) {
    mainWindow.webContents.send('update-available', {
      currentVersion: app.getVersion(),
      newVersion: info.version,
      releaseNotes: info.releaseNotes,
      releaseDate: info.releaseDate
    });
  }
});

autoUpdater.on('update-not-available', (info) => {
  console.log('Update not available. Current version is latest:', info.version);
});

autoUpdater.on('error', (err) => {
  console.error('Error in auto-updater:', err);
  console.log('Falling back to manual update check...');
  
  // Fallback to manual update check
  checkForManualUpdate().then(updateInfo => {
    if (updateInfo && mainWindow) {
      mainWindow.webContents.send('manual-update-available', updateInfo);
    }
  }).catch(fallbackErr => {
    console.error('Fallback update check also failed:', fallbackErr);
    if (mainWindow) {
      mainWindow.webContents.send('update-error', err.message);
    }
  });
});

autoUpdater.on('download-progress', (progressObj) => {
  console.log(`Download speed: ${progressObj.bytesPerSecond} - Downloaded ${progressObj.percent}%`);
  if (mainWindow) {
    mainWindow.webContents.send('update-download-progress', {
      percent: progressObj.percent,
      transferred: progressObj.transferred,
      total: progressObj.total
    });
  }
});

autoUpdater.on('update-downloaded', (info) => {
  console.log('Update downloaded:', info.version);
  if (mainWindow) {
    mainWindow.webContents.send('update-downloaded', {
      version: info.version
    });
  }
});

// Fallback manual update checker using GitHub Pages hosted package.json
async function checkForManualUpdate() {
  return new Promise((resolve, reject) => {
    const currentVersion = app.getVersion();
    const packageJsonUrl = 'https://tdoukinitsas.github.io/liveplay/package.json';
    
    console.log('Checking for updates manually at:', packageJsonUrl);
    console.log('Current version:', currentVersion);
    
    https.get(packageJsonUrl, (res) => {
      let data = '';
      
      res.on('data', (chunk) => {
        data += chunk;
      });
      
      res.on('end', () => {
        try {
          const packageData = JSON.parse(data);
          const latestVersion = packageData.version;
          
          console.log('Latest version from package.json:', latestVersion);
          
          // Simple version comparison
          if (compareVersions(latestVersion, currentVersion) > 0) {
            console.log('New version available:', latestVersion);
            resolve({
              currentVersion,
              newVersion: latestVersion,
              downloadUrl: 'https://tdoukinitsas.github.io/liveplay/',
              isManualUpdate: true
            });
          } else {
            console.log('No update available');
            resolve(null);
          }
        } catch (error) {
          console.error('Error parsing package.json:', error);
          reject(error);
        }
      });
    }).on('error', (error) => {
      console.error('Error fetching package.json:', error);
      reject(error);
    });
  });
}

// Simple version comparison (e.g., "1.2.3" vs "1.2.4")
function compareVersions(v1, v2) {
  const parts1 = v1.split('.').map(Number);
  const parts2 = v2.split('.').map(Number);
  
  for (let i = 0; i < Math.max(parts1.length, parts2.length); i++) {
    const num1 = parts1[i] || 0;
    const num2 = parts2[i] || 0;
    
    if (num1 > num2) return 1;
    if (num1 < num2) return -1;
  }
  
  return 0;
}

function createWindow() {
  // `useContentSize: true` rides in MIN_WINDOW, so these are CONTENT dimensions
  // and the saved size has to be a content size too — see trackWindowBounds.
  // A stored size below MIN_WINDOW needs no guarding here: Electron clamps a
  // requested size up to minWidth/minHeight itself.
  const { maximized, ...geom } = savedWindowBounds('main', { width: 1400, height: 900 });
  mainWindow = new BrowserWindow({
    ...geom,
    // The floor the layouts are designed against. The mixer's channel view in
    // particular assumes it can put a channel column, an EQ and a dynamics
    // section side by side; below this it degrades to stacked, scrolling
    // sections, which works but is not what anyone should be running a show on.
    ...MIN_WINDOW,
    icon: path.join(__dirname, '../assets/icons/2x/app_icon_darkmode@2x.png'),
    webPreferences: {
      nodeIntegration: false,
      contextIsolation: true,
      preload: path.join(__dirname, 'preload.js'),
      webSecurity: false // Allow loading local files
    },
    show: false
  });
  if (maximized) mainWindow.maximize();
  trackWindowBounds('main', mainWindow, { contentSize: true });

  // Use the global isDevMode flag
  if (isDevMode) {
    mainWindow.loadURL('http://localhost:3000');
    // Open DevTools in development
    mainWindow.webContents.openDevTools();
  } else {
    // In production, load the generated static files
    const indexPath = path.join(__dirname, '../.output/public/index.html');
    console.log('Loading production index from:', indexPath);
    console.log('File exists:', fs.existsSync(indexPath));
    
    mainWindow.loadFile(indexPath).catch(err => {
      console.error('Failed to load index.html:', err);
    });
  }

  // Log any loading errors
  mainWindow.webContents.on('did-fail-load', (event, errorCode, errorDescription) => {
    console.error('Failed to load:', errorCode, errorDescription);
  });

  mainWindow.once('ready-to-show', () => {
    mainWindow.show();
    
    // Check for updates only in production
    if (!isDevMode) {
      // Wait a bit for the window to fully load before checking updates
      setTimeout(() => {
        autoUpdater.checkForUpdates().catch(err => {
          console.error('Failed to check for updates:', err);
        });
      }, 3000);
    }
  });

  // Veto the first close so the renderer can run its quit-confirmation
  // flow (unsaved changes → optionally shut the local audio server down).
  // The renderer responds via `app:confirm-quit`, which sets quitConfirmed
  // and calls app.quit() — re-entering here with the veto lifted. If the
  // renderer is gone (crash / already destroyed) we let the close proceed.
  mainWindow.on('close', (e) => {
    if (quitConfirmed) return;
    if (!mainWindow || mainWindow.webContents.isDestroyed()) return;
    e.preventDefault();
    mainWindow.webContents.send('app:request-quit');
  });

  mainWindow.on('closed', () => {
    mainWindow = null;
  });

  createMenu('en', isDevMode);
  startAPIServer();
}

// Create detached cart player window
function createCartPlayerWindow() {
  if (cartPlayerWindow) {
    cartPlayerWindow.focus();
    return;
  }

  // No `useContentSize` on this one, so its numbers are OUTER bounds and it is
  // tracked that way. Mixing the two up is what makes a window creep.
  const { maximized, ...geom } = savedWindowBounds('cart', { width: 900, height: 700 });
  cartPlayerWindow = new BrowserWindow({
    ...geom,
    minWidth: 380,
    minHeight: 400,
    title: 'LivePlay - Cart Player',
    icon: path.join(__dirname, '../assets/icons/2x/app_icon_darkmode@2x.png'),
    webPreferences: {
      nodeIntegration: false,
      contextIsolation: true,
      preload: path.join(__dirname, 'preload.js'),
      webSecurity: false
    }
  });
  if (maximized) cartPlayerWindow.maximize();
  trackWindowBounds('cart', cartPlayerWindow);

  if (isDevMode) {
    cartPlayerWindow.loadURL('http://localhost:3000/?cartWindow=1');
  } else {
    const indexPath = path.join(__dirname, '../.output/public/index.html');
    cartPlayerWindow.loadFile(indexPath, { query: { cartWindow: '1' } });
  }

  cartPlayerWindow.webContents.on('did-fail-load', (event, errorCode, errorDescription) => {
    console.error('[CartWindow] Failed to load:', errorCode, errorDescription);
  });

  cartPlayerWindow.on('closed', () => {
    cartPlayerWindow = null;
    if (mainWindow) {
      mainWindow.webContents.send('cart-player-window-closed');
    }
  });

  // Notify main window that cart window is open
  cartPlayerWindow.webContents.once('did-finish-load', () => {
    if (mainWindow) {
      mainWindow.webContents.send('cart-player-window-opened');
    }
  });
}

// Create detached mixer window.
//
// Unlike the cart window this one needs almost nothing from us: buses, meters
// and fader moves all travel over the renderer's own WebSocket to the audio
// server, which every window opens independently. The only thing it takes from
// the main window is the project's look (theme + accent) and the meter zone
// levels, which ride along on the existing cart-window project-data channel.
function createMixerWindow() {
  if (mixerWindow) {
    mixerWindow.focus();
    return;
  }

  // MIN_WINDOW again, so content dimensions again.
  const { maximized, ...geom } = savedWindowBounds('mixer', { width: 1440, height: 860 });
  mixerWindow = new BrowserWindow({
    ...geom,
    ...MIN_WINDOW,
    title: 'LivePlay - Mixer',
    icon: path.join(__dirname, '../assets/icons/2x/app_icon_darkmode@2x.png'),
    webPreferences: {
      nodeIntegration: false,
      contextIsolation: true,
      preload: path.join(__dirname, 'preload.js'),
      webSecurity: false
    }
  });
  if (maximized) mixerWindow.maximize();
  trackWindowBounds('mixer', mixerWindow, { contentSize: true });

  if (isDevMode) {
    mixerWindow.loadURL('http://localhost:3000/?mixerWindow=1');
  } else {
    const indexPath = path.join(__dirname, '../.output/public/index.html');
    mixerWindow.loadFile(indexPath, { query: { mixerWindow: '1' } });
  }

  mixerWindow.webContents.on('did-fail-load', (event, errorCode, errorDescription) => {
    console.error('[MixerWindow] Failed to load:', errorCode, errorDescription);
  });

  mixerWindow.on('closed', () => {
    mixerWindow = null;
    if (mainWindow && !mainWindow.webContents.isDestroyed()) {
      mainWindow.webContents.send('mixer-window-closed');
    }
  });

  mixerWindow.webContents.once('did-finish-load', () => {
    if (mainWindow && !mainWindow.webContents.isDestroyed()) {
      mainWindow.webContents.send('mixer-window-opened');
    }
  });
}

// Create state viewer window for debugging
function createStateViewerWindow() {
  if (stateViewerWindow) {
    stateViewerWindow.focus();
    return;
  }

  // Tracked like the rest, even though it is a dev-only debug window: somebody
  // watching state while reproducing a bug drags it out of the way of the thing
  // they are watching, and having to do that again on every open is the same
  // annoyance as the main window's, just to a smaller audience. Outer bounds.
  const { maximized, ...geom } = savedWindowBounds('stateViewer', { width: 1200, height: 800 });
  stateViewerWindow = new BrowserWindow({
    ...geom,
    title: 'LivePlay - Current State Viewer',
    icon: path.join(__dirname, '../assets/icons/2x/app_icon_darkmode@2x.png'),
    webPreferences: {
      nodeIntegration: false,
      contextIsolation: true,
      preload: path.join(__dirname, 'preload-state-viewer.js')
    }
  });
  if (maximized) stateViewerWindow.maximize();
  trackWindowBounds('stateViewer', stateViewerWindow);

  // Create a simple HTML page for the state viewer
  const stateViewerHTML = `
    <!DOCTYPE html>
    <html>
    <head>
      <meta charset="UTF-8">
      <title>LivePlay State Viewer</title>
      <style>
        * {
          margin: 0;
          padding: 0;
          box-sizing: border-box;
        }
        
        body {
          font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
          background: #1e1e1e;
          color: #d4d4d4;
          overflow: hidden;
          display: flex;
          flex-direction: column;
          height: 100vh;
        }
        
        .header {
          background: #252526;
          padding: 12px 20px;
          border-bottom: 1px solid #3e3e42;
          flex-shrink: 0;
        }
        
        h1 {
          font-size: 16px;
          font-weight: 600;
          color: #cccccc;
        }
        
        .container {
          flex: 1;
          overflow-y: auto;
          padding: 20px;
        }
        
        .state-section {
          margin-bottom: 24px;
          background: #252526;
          border: 1px solid #3e3e42;
          border-radius: 4px;
          overflow: hidden;
        }
        
        .section-header {
          background: #2d2d30;
          padding: 10px 16px;
          font-weight: 600;
          font-size: 13px;
          color: #cccccc;
          border-bottom: 1px solid #3e3e42;
          cursor: pointer;
          user-select: none;
          display: flex;
          justify-content: space-between;
          align-items: center;
        }
        
        .section-header:hover {
          background: #3e3e42;
        }
        
        .collapse-icon {
          font-size: 12px;
          transition: transform 0.2s;
        }
        
        .collapse-icon.collapsed {
          transform: rotate(-90deg);
        }
        
        .section-content {
          padding: 16px;
          overflow: hidden;
        }
        
        .section-content.collapsed {
          display: none;
        }
        
        pre {
          font-family: 'IBM Plex Mono', 'Consolas', monospace;
          font-size: 12px;
          line-height: 1.5;
          overflow-x: auto;
          white-space: pre;
        }
        
        /* JSON Syntax Highlighting */
        .json-key {
          color: #9cdcfe;
        }
        
        .json-string {
          color: #ce9178;
        }
        
        .json-number {
          color: #b5cea8;
        }
        
        .json-boolean {
          color: #569cd6;
        }
        
        .json-null {
          color: #569cd6;
        }
        
        .update-time {
          font-size: 11px;
          color: #858585;
          margin-top: 8px;
        }
        
        ::-webkit-scrollbar {
          width: 10px;
          height: 10px;
        }
        
        ::-webkit-scrollbar-track {
          background: #1e1e1e;
        }
        
        ::-webkit-scrollbar-thumb {
          background: #424242;
          border-radius: 5px;
        }
        
        ::-webkit-scrollbar-thumb:hover {
          background: #4e4e4e;
        }
      </style>
    </head>
    <body>
      <div class="header">
        <h1>LivePlay - Current State Viewer (Development Mode)</h1>
      </div>
      <div class="container" id="container"></div>
      
      <script>
        const collapsedSections = new Set();
        const scrollPositions = new Map();
        
        function syntaxHighlight(json) {
          json = JSON.stringify(json, null, 2);
          json = json.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
          return json.replace(/("(\\\\u[a-zA-Z0-9]{4}|\\\\[^u]|[^\\\\"])*"(\\s*:)?|\\b(true|false|null)\\b|-?\\d+(?:\\.\\d*)?(?:[eE][+\\-]?\\d+)?)/g, function (match) {
            let cls = 'json-number';
            if (/^"/.test(match)) {
              if (/:$/.test(match)) {
                cls = 'json-key';
              } else {
                cls = 'json-string';
              }
            } else if (/true|false/.test(match)) {
              cls = 'json-boolean';
            } else if (/null/.test(match)) {
              cls = 'json-null';
            }
            return '<span class="' + cls + '">' + match + '</span>';
          });
        }
        
        function toggleSection(sectionId) {
          const section = document.getElementById(sectionId);
          const icon = document.getElementById(sectionId + '-icon');
          const content = document.getElementById(sectionId + '-content');
          
          if (collapsedSections.has(sectionId)) {
            collapsedSections.delete(sectionId);
            content.classList.remove('collapsed');
            icon.classList.remove('collapsed');
          } else {
            // Save scroll position before collapsing
            scrollPositions.set(sectionId, content.scrollTop);
            collapsedSections.add(sectionId);
            content.classList.add('collapsed');
            icon.classList.add('collapsed');
          }
        }
        
        function updateState(state) {
          console.log('[State Viewer] updateState called with:', Object.keys(state));
          const container = document.getElementById('container');
          if (!container) {
            console.error('[State Viewer] Container not found!');
            return;
          }
          
          const currentScroll = container.scrollTop;
          
          // Save scroll positions for each section
          const sections = container.querySelectorAll('.section-content');
          sections.forEach(section => {
            if (section.id) {
              scrollPositions.set(section.id, section.scrollTop);
            }
          });
          
          let html = '';
          
          for (const [key, value] of Object.entries(state)) {
            const sectionId = 'section-' + key;
            const isCollapsed = collapsedSections.has(sectionId);
            
            html += \`
              <div class="state-section">
                <div class="section-header" onclick="toggleSection('\${sectionId}')">
                  <span>\${key.replace(/([A-Z])/g, ' $1').replace(/^./, str => str.toUpperCase())}</span>
                  <span class="collapse-icon \${isCollapsed ? 'collapsed' : ''}" id="\${sectionId}-icon">▼</span>
                </div>
                <div class="section-content \${isCollapsed ? 'collapsed' : ''}" id="\${sectionId}-content">
                  <pre>\${syntaxHighlight(value)}</pre>
                  <div class="update-time">Last updated: \${new Date().toLocaleTimeString()}</div>
                </div>
              </div>
            \`;
          }
          
          container.innerHTML = html;
          console.log('[State Viewer] Updated DOM with', Object.keys(state).length, 'sections');
          
          // Restore scroll positions
          container.scrollTop = currentScroll;
          scrollPositions.forEach((scrollTop, sectionId) => {
            const section = document.getElementById(sectionId);
            if (section) {
              section.scrollTop = scrollTop;
            }
          });
        }
        
        // Listen for state updates
        window.electronAPI.onStateUpdate((event, state) => {
          console.log('[State Viewer] Received state update:', Object.keys(state));
          updateState(state);
        });
        
        // Initial message
        console.log('[State Viewer] Initialized, waiting for updates...');
        updateState({
          message: 'Waiting for state updates from main application...'
        });
      </script>
    </body>
    </html>
  `;

  stateViewerWindow.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(stateViewerHTML));

  // Make sure the window is ready before we start receiving updates
  stateViewerWindow.webContents.once('did-finish-load', () => {
    console.log('[Main] State viewer window loaded and ready');
  });

  stateViewerWindow.on('closed', () => {
    stateViewerWindow = null;
  });
}

// Translation strings for menu (default: English)
// Dynamically load all locale files from the locales directory
function loadLocaleFiles() {
  const localesDir = path.join(__dirname, '../locales');
  const localeFiles = {};
  
  try {
    // Read all files in the locales directory
    const files = fs.readdirSync(localesDir);
    
    // Filter for JSON files and load them
    files.forEach(file => {
      if (file.endsWith('.json')) {
        const code = file.replace('.json', '');
        try {
          localeFiles[code] = require(path.join(localesDir, file));
          console.log(`Loaded locale: ${code}`);
        } catch (error) {
          console.error(`Failed to load locale ${code}:`, error);
        }
      }
    });
    
    console.log(`Loaded ${Object.keys(localeFiles).length} locale files`);
  } catch (error) {
    console.error('Failed to read locales directory:', error);
    // Fallback to English if directory read fails
    localeFiles.en = require('../locales/en.json');
  }
  
  return localeFiles;
}

const localeFiles = loadLocaleFiles();

// Build menu translations from locale files
const menuTranslations = Object.entries(localeFiles).reduce((acc, [code, data]) => {
  acc[code] = {
    file: data.menu.file,
    newProject: data.menu.newProject,
    openProject: data.menu.openProject,
    openRecent: data.menu.openRecent || 'Open Recent',
    clearRecentProjects: data.menu.clearRecentProjects || 'Clear Recently Opened',
    noRecentProjects: data.menu.noRecentProjects || 'No Recent Projects',
    saveProject: data.menu.saveProject,
    exportProject: data.menu.exportProject,
    importProject: data.menu.importProject,
    closeProject: data.menu.closeProject,
    openProjectFolder: data.menu.openProjectFolder,
    exit: data.menu.exit,
    view: data.menu.view,
    toggleDarkMode: data.menu.toggleDarkMode,
    changeAccentColor: data.menu.changeAccentColor,
    fullscreen: data.menu.fullscreen,
    language: data.menu.language,
    window: data.menu.window || 'Window',
    // The two detachable panels name themselves, rather than getting `menu.*`
    // copies — same reasoning as the Settings menu borrowing the page's strings.
    mixerTitle: data.mixer.title,
    cartTitle: data.cart.title,
    help: data.menu.help,
    about: data.menu.about,
    // The Settings menu borrows the PAGE's own strings rather than getting
    // `menu.*` copies of them: `settings.title` is what the page calls itself
    // and `settings.section*` are the labels already on its rail. Two spellings
    // of "Outputs" that can drift apart is exactly the split this menu exists
    // to close, and it means the menu needed no new locale keys at all.
    settings: data.settings.title,
    settingsSections: data.settings
  };
  return acc;
}, {});

// The Settings menu's items, in the rail's order.
//
// ⚠️ THIS LIST MIRRORS `SETTINGS_SECTIONS` in client/app/composables/
// useSettingsPage.ts, WHICH IS THE SOURCE OF TRUTH. The main process cannot
// import a TS composable out of the renderer, so a new pane is two edits: there
// and here. The labels cannot drift (they are resolved from the same
// `settings.section*` keys the rail uses), but the LIST can — and an id offered
// here that the registry does not know opens the default pane instead, because
// `useSettingsPage().open()` validates against the registry. Eleven panes as of
// the Mixer pane; keep the order the same as the rail's so the menu reads like
// the page.
const SETTINGS_MENU_SECTIONS = [
  { id: 'appearance', labelKey: 'sectionAppearance' },
  { id: 'playback',   labelKey: 'sectionPlayback'   },
  { id: 'audio',      labelKey: 'sectionAudio'      },
  { id: 'mixer',      labelKey: 'sectionMixer'      },
  { id: 'outputs',    labelKey: 'sectionOutputs'    },
  { id: 'keyboard',   labelKey: 'sectionKeyboard'   },
  { id: 'surfaces',   labelKey: 'sectionSurfaces'   },
  { id: 'project',    labelKey: 'sectionProject'    },
  { id: 'server',     labelKey: 'sectionServer'     },
  { id: 'users',      labelKey: 'sectionUsers'      },
  { id: 'about',      labelKey: 'sectionAbout'      },
];

let currentLocale = 'en';

// Build the File > Open Recent submenu from the persisted recent-projects
// list. Each entry sends its server-filesystem path to the renderer, which
// closes any open project (with an unsaved-changes guard) before loading it.
// A disabled placeholder is shown when the list is empty.
function buildRecentProjectsSubmenu(t) {
  const recent = readRecentProjects();
  if (recent.length === 0) {
    return [{ label: t.noRecentProjects, enabled: false }];
  }
  const items = recent.map((entry) => ({
    label: entry.name ? `${entry.name}  —  ${entry.path}` : entry.path,
    click: () => {
      if (mainWindow && !mainWindow.isDestroyed()) {
        mainWindow.webContents.send('menu-open-recent-project', entry.path);
      }
    },
  }));
  items.push(
    { type: 'separator' },
    {
      label: t.clearRecentProjects,
      click: () => { clearRecentProjects(); createMenu(currentLocale, isDevMode); },
    },
  );
  return items;
}

function createMenu(locale = 'en', isDev = false) {
  currentLocale = locale;
  const t = menuTranslations[locale] || menuTranslations.en;
  
  const template = [
    {
      label: t.file,
      submenu: [
        {
          label: t.newProject,
          accelerator: 'CmdOrCtrl+N',
          click: () => {
            mainWindow.webContents.send('menu-new-project');
          }
        },
        {
          label: t.openProject,
          accelerator: 'CmdOrCtrl+O',
          click: () => {
            mainWindow.webContents.send('menu-open-project');
          }
        },
        {
          label: t.openRecent,
          submenu: buildRecentProjectsSubmenu(t),
        },
        {
          label: t.saveProject,
          accelerator: 'CmdOrCtrl+S',
          click: () => {
            mainWindow.webContents.send('menu-save-project');
          }
        },
        { type: 'separator' },
        {
          label: t.exportProject,
          enabled: currentProject !== null,
          click: () => {
            mainWindow.webContents.send('menu-export-project');
          }
        },
        {
          label: t.importProject,
          click: () => {
            mainWindow.webContents.send('menu-import-project');
          }
        },
        { type: 'separator' },
        {
          label: t.openProjectFolder,
          click: () => {
            mainWindow.webContents.send('menu-open-project-folder');
          }
        },
        { type: 'separator' },
        {
          label: t.closeProject,
          accelerator: 'CmdOrCtrl+W',
          click: () => {
            mainWindow.webContents.send('menu-close-project');
          }
        },
        { type: 'separator' },
        {
          label: t.exit,
          accelerator: 'CmdOrCtrl+Q',
          click: () => {
            app.quit();
          }
        }
      ]
    },
    {
      label: t.view,
      submenu: [
        {
          label: t.toggleDarkMode,
          click: () => {
            mainWindow.webContents.send('menu-toggle-dark-mode');
          }
        },
        {
          label: t.changeAccentColor,
          click: () => {
            mainWindow.webContents.send('menu-change-accent-color');
          }
        },
        { type: 'separator' },
        {
          label: t.fullscreen,
          accelerator: 'F11',
          click: () => {
            const isFullScreen = mainWindow.isFullScreen();
            mainWindow.setFullScreen(!isFullScreen);
          }
        },
        { type: 'separator' },
        {
          label: t.language,
          submenu: Object.values(localeFiles).map((localeData) => ({
            label: localeData._metadata.nativeName,
            type: 'radio',
            checked: locale === localeData._metadata.code,
            click: () => {
              mainWindow.webContents.send('menu-change-language', localeData._metadata.code);
              createMenu(localeData._metadata.code, isDev);
            }
          }))
        },
        ...(isDev ? [
          { type: 'separator' },
          {
            label: 'Show Current State',
            accelerator: 'CmdOrCtrl+Shift+D',
            click: () => {
              createStateViewerWindow();
            }
          },
          { type: 'separator' },
          { role: 'reload' },
          { role: 'forceReload' },
          { 
          label: 'Toggle Developer Tools',
          accelerator: process.platform === 'darwin' ? 'Alt+Command+I' : 'Ctrl+Shift+I',
          click: () => {
            if (mainWindow && mainWindow.webContents) {
              mainWindow.webContents.toggleDevTools();
            }
          }
        }
        ] : [])
      ]
    },
    // Asked for 2026-09-27: a settings/preferences menu carrying every pane, so
    // the configuration has an OS-level way in and not only a header button.
    // Each item is a deep link — the same move P3d made for the mixer's output
    // map and P3e for About and the accent swatches — so the page stays the one
    // home for all of it and the menu is an address book, not a second surface.
    //
    // `CmdOrCtrl+,` is the conventional preferences accelerator on both
    // platforms, and it is free here: no other menu item claims it and the
    // client has no comma-key handling.
    {
      label: t.settings,
      submenu: [
        {
          label: t.settings,
          accelerator: 'CmdOrCtrl+,',
          click: () => {
            // No id: the renderer opens whatever DEFAULT_SETTINGS_SECTION is,
            // rather than this process holding a second opinion about it.
            mainWindow.webContents.send('menu-open-settings');
          }
        },
        { type: 'separator' },
        ...SETTINGS_MENU_SECTIONS.map((s) => ({
          // Same `|| fallback` shape the File menu's newer keys use: a label
          // that resolved to undefined would put a blank row in the menu, and
          // the id is at least a word the operator can act on.
          label: t.settingsSections[s.labelKey] || s.id,
          click: () => {
            mainWindow.webContents.send('menu-open-settings', s.id);
          }
        }))
      ]
    },
    // Asked for 2026-09-27 alongside the cart's docking controls: an OS-level
    // way to put either detachable panel into its own window.
    //
    // These call the window factories DIRECTLY rather than asking the renderer
    // to, because both already focus an existing window instead of making a
    // second one, and both already tell the main window they opened
    // (`cart-player-window-opened` / `mixer-window-opened`) — which is what sets
    // `cartDetached` / `mixerDetached` there. So one path serves the menu, the
    // panel's own Detach button and a reopen after closing, and the renderer
    // never has to be asked where the project folder is.
    {
      label: t.window,
      submenu: [
        {
          label: t.mixerTitle,
          click: () => {
            createMixerWindow();
          }
        },
        {
          label: t.cartTitle,
          click: () => {
            createCartPlayerWindow();
          }
        }
      ]
    },
    {
      label: t.help,
      submenu: [
        {
          label: t.about,
          click: () => {
            mainWindow.webContents.send('menu-show-about');
          }
        }
      ]
    }
  ];

  const menu = Menu.buildFromTemplate(template);
  Menu.setApplicationMenu(menu);
}

// IPC Handlers
ipcMain.handle('select-project-folder', async () => {
  const result = await dialog.showOpenDialog(mainWindow, {
    properties: ['openDirectory', 'createDirectory']
  });

  if (!result.canceled && result.filePaths.length > 0) {
    return result.filePaths[0];
  }
  return null;
});

ipcMain.handle('select-project-file', async () => {
  const result = await dialog.showOpenDialog(mainWindow, {
    properties: ['openFile'],
    filters: [{ name: 'LivePlay Project', extensions: ['liveplay'] }]
  });

  if (!result.canceled && result.filePaths.length > 0) {
    return result.filePaths[0];
  }
  return null;
});

ipcMain.handle('select-audio-files', async () => {
  const result = await dialog.showOpenDialog(mainWindow, {
    properties: ['openFile', 'multiSelections'],
    filters: [
      { name: 'Audio Files', extensions: ['mp3', 'wav', 'ogg', 'flac', 'm4a', 'aac'] }
    ]
  });

  if (!result.canceled && result.filePaths.length > 0) {
    return result.filePaths;
  }
  return null;
});

ipcMain.handle('read-file', async (event, filePath) => {
  try {
    const data = fs.readFileSync(filePath, 'utf8');
    return { success: true, data };
  } catch (error) {
    return { success: false, error: error.message };
  }
});

ipcMain.handle('read-audio-file', async (event, filePath) => {
  try {
    const data = fs.readFileSync(filePath);
    // Convert Node.js Buffer to ArrayBuffer
    const arrayBuffer = data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength);
    return { success: true, data: Array.from(new Uint8Array(arrayBuffer)) };
  } catch (error) {
    return { success: false, error: error.message };
  }
});

ipcMain.handle('write-file', async (event, filePath, data) => {
  try {
    fs.writeFileSync(filePath, data, 'utf8');
    return { success: true };
  } catch (error) {
    return { success: false, error: error.message };
  }
});

// Save dialog for the .lpa download flow. Returns the chosen absolute path
// or null on cancel. `defaultName` is the suggested filename (e.g. "MyShow.lpa").
ipcMain.handle('show-save-archive-dialog', async (event, defaultName) => {
  const result = await dialog.showSaveDialog(mainWindow, {
    title: 'Save Project Archive',
    defaultPath: defaultName || 'project.lpa',
    filters: [{ name: 'LivePlay Archive', extensions: ['lpa'] }],
  });
  if (result.canceled || !result.filePath) return null;
  return result.filePath;
});

// Save dialog for a JSON file the renderer then writes with write-file (the
// account export, for one). Generic rather than per-feature, so the next JSON
// export does not need a channel of its own. Returns the path or null.
ipcMain.handle('show-save-json-dialog', async (event, defaultName, title) => {
  const result = await dialog.showSaveDialog(mainWindow, {
    title: title || 'Save',
    defaultPath: defaultName || 'export.json',
    filters: [{ name: 'JSON', extensions: ['json'] }],
  });
  if (result.canceled || !result.filePath) return null;
  return result.filePath;
});

// Open dialog for the .lpa upload flow (client picks a .lpa from local disk
// to upload to a remote server for extraction). Returns the absolute path or
// null on cancel.
ipcMain.handle('show-open-archive-dialog', async () => {
  const result = await dialog.showOpenDialog(mainWindow, {
    title: 'Choose Project Archive',
    properties: ['openFile'],
    filters: [{ name: 'LivePlay Archive', extensions: ['lpa'] }],
  });
  if (result.canceled || result.filePaths.length === 0) return null;
  return result.filePaths[0];
});

// Write raw bytes (binary) to a local file. Used by the download flow to
// persist the .lpa blob streamed back from the server.
ipcMain.handle('write-binary-file', async (event, filePath, data) => {
  try {
    const buf = Buffer.from(new Uint8Array(data));
    fs.writeFileSync(filePath, buf);
    return { success: true };
  } catch (error) {
    return { success: false, error: error.message };
  }
});

ipcMain.handle('copy-file', async (event, source, destination) => {
  try {
    // Ensure destination directory exists
    const destDir = path.dirname(destination);
    if (!fs.existsSync(destDir)) {
      fs.mkdirSync(destDir, { recursive: true });
    }
    fs.copyFileSync(source, destination);
    return { success: true };
  } catch (error) {
    return { success: false, error: error.message };
  }
});

ipcMain.handle('ensure-directory', async (event, dirPath) => {
  try {
    if (!fs.existsSync(dirPath)) {
      fs.mkdirSync(dirPath, { recursive: true });
    }
    return { success: true };
  } catch (error) {
    return { success: false, error: error.message };
  }
});

ipcMain.handle('open-folder', async (event, folderPath) => {
  try {
    shell.openPath(folderPath);
    return { success: true };
  } catch (error) {
    return { success: false, error: error.message };
  }
});

// Open external URL in default browser
ipcMain.handle('open-external', async (event, url) => {
  try {
    await shell.openExternal(url);
    return { success: true };
  } catch (error) {
    return { success: false, error: error.message };
  }
});

// Update menu language from renderer
ipcMain.handle('update-menu-language', async (event, locale) => {
  createMenu(locale, isDevMode);
  return { success: true };
});

// Auto-updater IPC handlers
ipcMain.handle('check-for-updates', async () => {
  try {
    console.log('Manual update check requested');
    const result = await autoUpdater.checkForUpdates();
    return { success: true, updateInfo: result?.updateInfo };
  } catch (error) {
    console.error('Check for updates error:', error);
    console.log('Attempting fallback manual update check...');
    
    // Try fallback method
    try {
      const manualUpdateInfo = await checkForManualUpdate();
      if (manualUpdateInfo) {
        return { 
          success: true, 
          isManualUpdate: true,
          updateInfo: manualUpdateInfo 
        };
      } else {
        return { success: true, updateInfo: null };
      }
    } catch (fallbackError) {
      console.error('Fallback update check error:', fallbackError);
      return { success: false, error: error.message };
    }
  }
});

ipcMain.handle('download-update', async () => {
  try {
    await autoUpdater.downloadUpdate();
    return { success: true };
  } catch (error) {
    console.error('Download update error:', error);
    return { success: false, error: error.message };
  }
});

ipcMain.handle('install-update', () => {
  // The user already opted into the update, so bypass the close veto /
  // quit-confirmation dialog and let electron-updater quit + relaunch.
  quitConfirmed = true;
  autoUpdater.quitAndInstall(false, true);
});

ipcMain.handle('get-app-version', () => {
  return app.getVersion();
});

ipcMain.handle('get-system-locale', () => {
  // Get the system locale from Electron
  const systemLocale = app.getLocale(); // Returns locale like 'en-US', 'es-ES', 'fr-FR', etc.
  
  // Extract just the language code (e.g., 'en' from 'en-US')
  const languageCode = systemLocale.split('-')[0].toLowerCase();
  
  return languageCode;
});

ipcMain.handle('get-available-locales', () => {
  // Return list of available locale codes and metadata
  return Object.keys(localeFiles).map(code => ({
    code,
    name: localeFiles[code]._metadata.nativeName,
    direction: localeFiles[code]._metadata.direction
  }));
});

ipcMain.handle('get-locale-data', (event, localeCode) => {
  // Return the full locale data for a specific locale
  if (localeCode in localeFiles) {
    return localeFiles[localeCode];
  }
  // Fallback to English if locale not found
  return localeFiles.en;
});

ipcMain.handle('set-current-project', async (event, projectPath) => {
  currentProject = projectPath;
  // Rebuild menu to update enabled/disabled state of menu items
  createMenu(currentLocale, isDevMode);
  return { success: true };
});

// Export project to .lpa archive
ipcMain.handle('export-project', async (event, projectFolderPath, projectName = null) => {
  try {
    const archiver = require('archiver');
    // Use provided project name or fall back to folder name
    const defaultName = projectName || path.basename(projectFolderPath);
    
    // Show save dialog for .lpa file
    const result = await dialog.showSaveDialog(mainWindow, {
      title: 'Export Project',
      defaultPath: `${defaultName}.lpa`,
      filters: [
        { name: 'LivePlay Archive', extensions: ['lpa'] }
      ]
    });

    if (result.canceled || !result.filePath) {
      return { success: false, canceled: true };
    }

    const outputPath = result.filePath;
    const fileName = path.basename(outputPath);
    const output = fs.createWriteStream(outputPath);
    const archive = archiver('zip', { zlib: { level: 9 } });

    return new Promise((resolve, reject) => {
      let totalBytes = 0;
      let processedBytes = 0;

      output.on('close', () => {
        event.sender.send('export-progress', { percentage: 100, fileName });
        resolve({
          success: true,
          path: outputPath,
          size: archive.pointer()
        });
      });

      archive.on('error', (err) => {
        reject({ success: false, error: err.message });
      });

      // Track progress by monitoring data being written
      archive.on('data', (chunk) => {
        processedBytes += chunk.length;
        if (totalBytes > 0) {
          const percentage = Math.min(99, Math.round((processedBytes / totalBytes) * 100));
          event.sender.send('export-progress', { percentage, fileName });
        }
      });

      archive.pipe(output);
      
      // Calculate total size first
      const calculateSize = (dirPath) => {
        let size = 0;
        const files = fs.readdirSync(dirPath);
        for (const file of files) {
          const filePath = path.join(dirPath, file);
          const stats = fs.statSync(filePath);
          if (stats.isDirectory()) {
            size += calculateSize(filePath);
          } else {
            size += stats.size;
          }
        }
        return size;
      };
      
      totalBytes = calculateSize(projectFolderPath);
      event.sender.send('export-progress', { percentage: 0, fileName });
      
      // Add the entire project folder to the archive
      archive.directory(projectFolderPath, false);
      
      archive.finalize();
    });
  } catch (error) {
    console.error('Export error:', error);
    return { success: false, error: error.message };
  }
});

// Import project from .lpa archive
ipcMain.handle('import-project', async (event) => {
  try {
    const extractZip = require('extract-zip');
    
    // Show open dialog for .lpa file
    const fileResult = await dialog.showOpenDialog(mainWindow, {
      title: 'Import Project',
      properties: ['openFile'],
      filters: [
        { name: 'LivePlay Archive', extensions: ['lpa'] }
      ]
    });

    if (fileResult.canceled || fileResult.filePaths.length === 0) {
      return { success: false, canceled: true };
    }

    const archivePath = fileResult.filePaths[0];
    const fileName = path.basename(archivePath);

    // Show folder dialog for extraction location
    const folderResult = await dialog.showOpenDialog(mainWindow, {
      title: 'Choose Where to Extract the Project',
      properties: ['openDirectory', 'createDirectory']
    });

    if (folderResult.canceled || folderResult.filePaths.length === 0) {
      return { success: false, canceled: true };
    }

    const extractPath = folderResult.filePaths[0];

    // Send initial progress
    event.sender.send('import-progress', { percentage: 0, fileName });

    // Extract the archive with progress updates
    await extractZip(archivePath, { 
      dir: extractPath,
      onEntry: (entry, zipfile) => {
        const percentage = Math.round((zipfile.entriesRead / zipfile.entryCount) * 100);
        event.sender.send('import-progress', { percentage, fileName });
      }
    });

    // Send completion
    event.sender.send('import-progress', { percentage: 100, fileName });

    // Find all .liveplay files in the extracted folder
    const files = fs.readdirSync(extractPath);
    const projectFiles = files.filter(file => file.endsWith('.liveplay'));

    if (projectFiles.length === 0) {
      return { success: false, error: 'No .liveplay file found in archive' };
    }

    // If multiple project files found, return them for user selection
    if (projectFiles.length > 1) {
      return {
        success: true,
        multipleProjects: true,
        projectFiles,
        extractPath
      };
    }

    // Single project file - return its path directly
    const projectPath = path.join(extractPath, projectFiles[0]);

    return {
      success: true,
      projectPath,
      extractPath
    };
  } catch (error) {
    console.error('Import error:', error);
    return { success: false, error: error.message };
  }
});

// Import project from specific .lpa file (for double-click file association)
ipcMain.handle('import-lpa-file', async (event, archivePath) => {
  try {
    const extractZip = require('extract-zip');
    const fileName = path.basename(archivePath);

    // Show folder dialog for extraction location
    const folderResult = await dialog.showOpenDialog(mainWindow, {
      title: 'Choose Where to Extract the Project',
      properties: ['openDirectory', 'createDirectory']
    });

    if (folderResult.canceled || folderResult.filePaths.length === 0) {
      return { success: false, canceled: true };
    }

    const extractPath = folderResult.filePaths[0];

    // Send initial progress
    event.sender.send('import-progress', { percentage: 0, fileName });

    // Extract the archive with progress updates
    await extractZip(archivePath, { 
      dir: extractPath,
      onEntry: (entry, zipfile) => {
        const percentage = Math.round((zipfile.entriesRead / zipfile.entryCount) * 100);
        event.sender.send('import-progress', { percentage, fileName });
      }
    });

    // Send completion
    event.sender.send('import-progress', { percentage: 100, fileName });

    // Find all .liveplay files in the extracted folder
    const files = fs.readdirSync(extractPath);
    const projectFiles = files.filter(file => file.endsWith('.liveplay'));

    if (projectFiles.length === 0) {
      return { success: false, error: 'No .liveplay file found in archive' };
    }

    // If multiple project files found, return them for user selection
    if (projectFiles.length > 1) {
      return {
        success: true,
        multipleProjects: true,
        projectFiles,
        extractPath
      };
    }

    // Single project file - return its path directly
    const projectPath = path.join(extractPath, projectFiles[0]);

    return {
      success: true,
      projectPath,
      extractPath
    };
  } catch (error) {
    console.error('Import LPA file error:', error);
    return { success: false, error: error.message };
  }
});

// Receive full project data from renderer to power HTTP API GET/PATCH endpoints
// Only sync from the main window, not from the detached cart window (to avoid feedback loops)
ipcMain.on('sync-project-data', (event, projectData) => {
  // Check if this sync is coming from the main window (not the cart window)
  const isFromMainWindow = event.sender === mainWindow?.webContents;

  if (isFromMainWindow) {
    currentProjectData = projectData;
    // Keep the menu's "Export Project" / similar items in sync with whether
    // a project is actually open in the renderer. The server-backed flow
    // never calls set-current-project explicitly, so without this the menu
    // gate (enabled: currentProject !== null) would stay stuck off forever
    // and File > Export Project would appear greyed out even with a project
    // loaded. Rebuild the menu only when the open/closed transition flips,
    // since createMenu() is not free.
    const nextOpen = projectData
      ? (projectData.folderPath || projectData.name || 'open')
      : null;
    const wasOpen = currentProject !== null;
    const isOpen  = nextOpen   !== null;
    if (wasOpen !== isOpen) {
      currentProject = nextOpen;
      createMenu(currentLocale, isDevMode);
    } else {
      currentProject = nextOpen;
    }
    // Forward project updates to the detached windows if open. The mixer
    // window takes only the theme and meter levels off this, but it is the
    // same payload, so it rides the same channel.
    if (projectData) {
      for (const win of [cartPlayerWindow, mixerWindow]) {
        if (win && !win.webContents.isDestroyed()) {
          win.webContents.send('cart-window-project-update', projectData);
        }
      }
    }
  }
  // Silently ignore syncs from the cart window to prevent feedback loops
});

// Cart player window IPC handlers
ipcMain.handle('open-cart-player-window', () => {
  createCartPlayerWindow();
});

ipcMain.on('cart-player-window-attach', () => {
  if (cartPlayerWindow) {
    cartPlayerWindow.close();
  }
});

// Mixer window IPC handlers
ipcMain.handle('open-mixer-window', () => {
  createMixerWindow();
});

ipcMain.on('mixer-window-attach', () => {
  if (mixerWindow) {
    mixerWindow.close();
  }
});

// UI mode (edit / "show mode") sync across windows. Each renderer owns its
// own state, so broadcast a change from any window to every other window so
// the detached cart player stays in lockstep with the main window.
ipcMain.on('ui-mode-changed', (event, mode) => {
  for (const win of BrowserWindow.getAllWindows()) {
    if (win.webContents.id === event.sender.id) continue;
    if (win.webContents && !win.webContents.isDestroyed()) {
      win.webContents.send('ui-mode-set', mode);
    }
  }
});

ipcMain.handle('get-cart-window-project-data', () => {
  return currentProjectData || null;
});

// Receive response from renderer for pending PATCH API requests
ipcMain.on('api-response', (event, data) => {
  const { requestId, ...result } = data;
  const pending = pendingApiRequests.get(requestId);
  if (pending) {
    pendingApiRequests.delete(requestId);
    pending.resolve(result);
  }
});

// State viewer: Receive state updates from renderer and forward to state viewer window
ipcMain.on('update-app-state', (event, state) => {
  //console.log('[Main] Received state update, viewer window exists:', !!stateViewerWindow);
  if (stateViewerWindow && !stateViewerWindow.isDestroyed()) {
    // Make sure webContents is ready
    if (stateViewerWindow.webContents && !stateViewerWindow.webContents.isDestroyed()) {
      console.log('[Main] Forwarding state to viewer window');
      stateViewerWindow.webContents.send('state-update', state);
    }
  }
});

// Check if dev mode is enabled
ipcMain.handle('is-dev-mode', () => {
  return isDevMode;
});

// Check FFmpeg availability
ipcMain.handle('check-ffmpeg', async () => {
  return {
    available: ffmpegAvailable,
    path: ffmpegPath || null
  };
});

// Waveform generation
ipcMain.handle('generate-waveform', async (event, audioFilePath, outputPath) => {
  return new Promise((resolve, reject) => {
    console.log('Generating waveform for:', audioFilePath);
    console.log('Output path:', outputPath);
    
    // Ensure output directory exists
    const outputDir = path.dirname(outputPath);
    if (!fs.existsSync(outputDir)) {
      fs.mkdirSync(outputDir, { recursive: true });
    }
    
    // Use the detected ffmpeg path
    if (ffmpegPath) {
      ffmpeg.setFfmpegPath(ffmpegPath);
    }
    
    // First, get the duration
    ffmpeg.ffprobe(audioFilePath, (err, metadata) => {
      if (err) {
        console.error('FFprobe error:', err);
        reject(err);
        return;
      }
      
      const duration = metadata.format.duration;
      if (!duration) {
        reject(new Error('Could not determine audio duration'));
        return;
      }
      
      // Calculate samples: 10 per second
      const targetSamples = Math.ceil(duration * 10);
      const samples = [];
      const tempOutput = outputPath + '.temp.wav';
      
      // Extract raw audio data
      ffmpeg(audioFilePath)
        .audioChannels(1)
        .audioFrequency(8000) // Lower frequency for smaller data
        .format('s16le')
        .on('error', (err) => {
          console.error('FFmpeg waveform error:', err);
          reject(err);
        })
        .on('end', () => {
          // Read the temp file and process samples
          try {
            if (fs.existsSync(tempOutput)) {
              const buffer = fs.readFileSync(tempOutput);
              
              // Process samples to get exactly 10 per second
              const sampleInterval = Math.floor(buffer.length / (targetSamples * 2)); // 2 bytes per sample
              
              for (let i = 0; i < buffer.length - 1 && samples.length < targetSamples; i += sampleInterval * 2) {
                const sample = buffer.readInt16LE(i) / 32768.0; // Normalize to -1 to 1
                samples.push(Math.abs(sample));
              }
              
              // Clean up temp file
              fs.unlinkSync(tempOutput);
              
              // Save waveform data (including duration for convenience)
              const waveformData = {
                peaks: samples,
                sampleRate: 10, // 10 samples per second
                duration: duration // Include duration in seconds
              };
              
              fs.writeFileSync(outputPath, JSON.stringify(waveformData));
              console.log('Waveform generated successfully:', samples.length, 'samples @10/sec for', duration.toFixed(2), 'seconds');
              
              resolve({ success: true });
            } else {
              reject(new Error('Temporary audio file not created'));
            }
          } catch (error) {
            console.error('Error processing waveform:', error);
            reject(error);
          }
        })
        .save(tempOutput);
    });
  });
});

// YouTube Search Handler
ipcMain.handle('search-youtube', async (event, query) => {
  try {
    const result = await youtubesearchapi.GetListByKeyword(query, false, 20, [{ type: 'video' }]);
    
    // Format results
    const videos = result.items.map(item => ({
      id: item.id,
      title: item.title,
      thumbnail: item.thumbnail.thumbnails[item.thumbnail.thumbnails.length - 1].url,
      channelTitle: item.channelTitle,
      length: item.length?.simpleText || ''
    }));
    
    return videos;
  } catch (error) {
    console.error('YouTube search error:', error);
    throw new Error('YouTube search failed.');
  }
});

// ---- YouTube import format --------------------------------------------
// What a YouTube download is saved as. A machine setting, like the server
// connection, because it is this machine's yt-dlp doing the work.
//
// The 48 kHz default is not arbitrary: YouTube's best audio stream is Opus,
// which is always 48 kHz, and the engine mixes at 48 kHz by default, so the
// file goes from YouTube to the output without a resampling step anywhere.
// WAV rather than MP3 so it is decoded once rather than re-encoded lossily.
const YOUTUBE_SETTINGS_FILENAME = 'youtube-import.json';
const YOUTUBE_SAMPLE_RATES = { wav: [44100, 48000, 96000], mp3: [44100, 48000] };
const YOUTUBE_MP3_BITRATES = [128, 192, 256, 320];
const YOUTUBE_DEFAULTS = { format: 'wav', sampleRate: 48000, mp3Bitrate: 320 };

function youtubeSettingsPath() {
  return path.join(app.getPath('userData'), YOUTUBE_SETTINGS_FILENAME);
}

// Anything unknown falls back to the default rather than reaching yt-dlp's
// command line. MP3 tops out at 48 kHz, so a WAV-only rate is clamped.
function normaliseYouTubeSettings(raw) {
  const format = raw?.format === 'mp3' ? 'mp3' : 'wav';
  const rates = YOUTUBE_SAMPLE_RATES[format];
  const sampleRate = rates.includes(raw?.sampleRate) ? raw.sampleRate
    : rates.includes(YOUTUBE_DEFAULTS.sampleRate) ? YOUTUBE_DEFAULTS.sampleRate : rates[rates.length - 1];
  const mp3Bitrate = YOUTUBE_MP3_BITRATES.includes(raw?.mp3Bitrate) ? raw.mp3Bitrate : YOUTUBE_DEFAULTS.mp3Bitrate;
  return { format, sampleRate, mp3Bitrate };
}

function readYouTubeSettings() {
  try {
    return normaliseYouTubeSettings(JSON.parse(fs.readFileSync(youtubeSettingsPath(), 'utf-8')));
  } catch {
    return { ...YOUTUBE_DEFAULTS };
  }
}

ipcMain.handle('youtube-settings:get', () => ({
  ...readYouTubeSettings(),
  options: { sampleRates: YOUTUBE_SAMPLE_RATES, mp3Bitrates: YOUTUBE_MP3_BITRATES },
}));

ipcMain.handle('youtube-settings:set', (_e, incoming) => {
  const next = normaliseYouTubeSettings({ ...readYouTubeSettings(), ...incoming });
  try {
    fs.writeFileSync(youtubeSettingsPath(), JSON.stringify(next, null, 2));
  } catch (e) {
    console.error('[youtube] failed to save import settings:', e);
  }
  return next;
});

// YouTube Download Handler
ipcMain.handle('download-youtube-audio', async (event, videoId, title, projectFolderPath) => {
  return new Promise(async (resolve, reject) => {
    const yt = readYouTubeSettings();
    console.log('YouTube download - Project folder path:', projectFolderPath);
    
    const outputPath = path.join(projectFolderPath, 'media');
    console.log('YouTube download - Output path:', outputPath);
    
    // Ensure output directory exists
    if (!fs.existsSync(outputPath)) {
      fs.mkdirSync(outputPath, { recursive: true });
      console.log('Created media directory:', outputPath);
    }
    
    // Clean filename
    const sanitizedTitle = title.replace(/[<>:"/\\|?*]/g, '').substring(0, 200);
    const fileName = `${sanitizedTitle}.${yt.format}`;
    const outputTemplate = path.join(outputPath, sanitizedTitle);
    
    console.log('YouTube download - Output template:', outputTemplate);
    
    const videoUrl = `https://www.youtube.com/watch?v=${videoId}`;
    
    console.log(`Starting YouTube download: ${videoId} -> ${fileName}`);
    console.log(`Video URL: ${videoUrl}`);
    
    // Wait for yt-dlp to be ready (with timeout)
    if (!ytDlpReady) {
      console.log('Waiting for yt-dlp to initialize...');
      let attempts = 0;
      while (!ytDlpReady && attempts < 30) { // Wait up to 30 seconds
        await new Promise(resolve => setTimeout(resolve, 1000));
        attempts++;
      }
      
      if (!ytDlpReady) {
        reject(new Error('yt-dlp initialization timed out. Please try again.'));
        return;
      }
    }
    
    if (!ytDlpPath) {
      reject(new Error('yt-dlp binary path not available. Please restart the application.'));
      return;
    }
    
    if (!ffmpegAvailable) {
      reject(new Error('Could not start the bundled FFmpeg. Restart LivePlay and try again.'));
      return;
    }
    
    try {
      // Create YTDlpWrap instance with the binary path
      const ytDlp = new YTDlpWrap(ytDlpPath);
      
      // Build yt-dlp arguments
      const args = [
        videoUrl,
        '-f', 'bestaudio',
        '--extract-audio',
        '--audio-format', yt.format,
        // yt-dlp passes these to the ffmpeg run that extracts the audio.
        '--postprocessor-args', `ExtractAudio:-ar ${yt.sampleRate}`,
        ...(yt.format === 'mp3' ? ['--audio-quality', `${yt.mp3Bitrate}K`] : []),
        '-o', outputTemplate + '.%(ext)s',
        '--no-playlist',
        '--progress',
        '--newline' // Force progress on new lines for easier parsing
      ];
      
      // Add ffmpeg path if we have it
      if (ffmpegPath) {
        args.push('--ffmpeg-location', ffmpegPath);
      }

      // Give deno a brief chance to finish initialising on first launch (the
      // ~40MB download may still be in flight), then enable it as yt-dlp's JS
      // runtime so YouTube's nsig challenge can be solved. Non-fatal: if deno
      // isn't ready we fall back to JS-runtime-free extraction.
      if (!denoReady) {
        let denoAttempts = 0;
        while (!denoReady && denoAttempts < 15) {
          await new Promise(resolve => setTimeout(resolve, 1000));
          denoAttempts++;
        }
      }
      if (denoReady && denoPath) {
        args.push('--js-runtimes', `deno:${denoPath}`);
      } else {
        console.warn('Proceeding without a JS runtime; some videos may fail to extract.');
      }

      console.log('Running yt-dlp with args:', args);
      console.log('yt-dlp path:', ytDlpPath);
      
      // Use spawn to get a proper ChildProcess
      const { spawn } = require('child_process');
      const downloadProcess = spawn(ytDlpPath, args);
      
      // Check if downloadProcess is valid
      if (!downloadProcess || !downloadProcess.stdout) {
        throw new Error('Failed to start yt-dlp process');
      }
      
      let lastProgress = 0;
      let stderrBuffer = '';

      // Track progress by parsing stdout
      downloadProcess.stdout.on('data', (data) => {
        const output = data.toString();
        
        // Parse download progress
        const downloadMatch = output.match(/\[download\]\s+(\d+\.?\d*)%/);
        if (downloadMatch) {
          const percentage = parseFloat(downloadMatch[1]);
          if (percentage > lastProgress) {
            lastProgress = percentage;
            event.sender.send('youtube-download-progress', {
              videoId,
              percentage: percentage,
              status: percentage < 100 ? 'downloading' : 'converting'
            });
          }
        }
        
        // Check for post-processing
        if (output.includes('[ExtractAudio]') || output.includes('Destination:')) {
          event.sender.send('youtube-download-progress', {
            videoId,
            percentage: 95,
            status: 'converting'
          });
        }
      });
      
      downloadProcess.stderr.on('data', (data) => {
        const errorOutput = data.toString();
        // yt-dlp uses stderr for normal output as well as errors. Keep the
        // tail of everything so we can report the real cause on failure.
        stderrBuffer += errorOutput;
        if (stderrBuffer.length > 8000) {
          stderrBuffer = stderrBuffer.slice(-8000);
        }
        if (errorOutput.includes('ERROR')) {
          console.error('yt-dlp error:', errorOutput);
        }
      });
      
      downloadProcess.on('error', (error) => {
        console.error('yt-dlp process error:', error);
        reject(new Error(`Download process failed: ${error.message}`));
      });
      
      downloadProcess.on('close', (code) => {
        console.log(`yt-dlp process closed with code: ${code}`);
        
        if (code !== 0) {
          // Surface the real reason from yt-dlp's stderr instead of just the code.
          const errorLines = stderrBuffer
            .split('\n')
            .filter(line => line.includes('ERROR') || line.includes('error:'))
            .map(line => line.trim());
          const detail = errorLines.length > 0
            ? errorLines.join('\n')
            : stderrBuffer.trim().split('\n').slice(-5).join('\n');

          console.error('yt-dlp failed. Full stderr:\n', stderrBuffer);

          let hint = '';
          if (/Sign in to confirm|not a bot|cookies/i.test(stderrBuffer)) {
            hint = ' (YouTube requires sign-in or bot verification for this video.)';
          } else if (/Video unavailable|Private video|members-only|age/i.test(stderrBuffer)) {
            hint = ' (This video is unavailable, private, age-restricted, or members-only.)';
          } else if (/Requested format is not available/i.test(stderrBuffer)) {
            hint = ' (No downloadable audio format was found for this video.)';
          } else if (/Unable to extract|nsig|player|update.*yt-dlp/i.test(stderrBuffer)) {
            hint = ' (yt-dlp may be out of date. Restart the app to update it.)';
          }

          reject(new Error(`yt-dlp exited with code ${code}.${hint}${detail ? '\n\n' + detail : ''}`));
          return;
        }
        
        console.log(`Download completed: ${fileName}`);
        
        // Find the actual downloaded file (yt-dlp might use URL encoding)
        const expectedFile = path.join(outputPath, fileName);
        let actualFile = expectedFile;
        
        // Check if file exists with expected name
        if (!fs.existsSync(expectedFile)) {
          // Try to find it with URL-encoded name or other variations
          const files = fs.readdirSync(outputPath);
          const baseName = sanitizedTitle;
          
          // Look for files that match the base name (case-insensitive, with any encoding)
          const matchingFile = files.find(f => {
            const decoded = decodeURIComponent(f);
            return decoded.toLowerCase().startsWith(baseName.toLowerCase()) && f.endsWith(`.${yt.format}`);
          });
          
          if (matchingFile) {
            actualFile = path.join(outputPath, matchingFile);
            console.log('Found downloaded file:', matchingFile);
            
            // Rename to expected filename if different
            if (matchingFile !== fileName) {
              try {
                fs.renameSync(actualFile, expectedFile);
                actualFile = expectedFile;
                console.log('Renamed file to:', fileName);
              } catch (renameError) {
                console.error('Failed to rename file:', renameError);
              }
            }
          } else {
            console.error('Could not find downloaded file. Files in directory:', files);
            reject(new Error('The downloaded file could not be found.'));
            return;
          }
        }
        
        // Send 100% progress
        event.sender.send('youtube-download-progress', {
          videoId,
          percentage: 100,
          status: 'completed'
        });
        
        resolve({
          success: true,
          file: actualFile,
          fileName: path.basename(actualFile),
          title: sanitizedTitle
        });
      });
      
    } catch (error) {
      console.error('YouTube download error:', error);
      console.error('Error stack:', error.stack);
      
      // Clean up partial file
      const outputFile = path.join(outputPath, fileName);
      if (fs.existsSync(outputFile)) {
        try {
          fs.unlinkSync(outputFile);
        } catch (e) {
          console.error('Failed to clean up file:', e);
        }
      }
      
      reject(new Error(`Download failed: ${error.message}`));
    }
  });
});

// Register custom protocol for app
if (process.defaultApp) {
  if (process.argv.length >= 2) {
    app.setAsDefaultProtocolClient('liveplay', process.execPath, [path.resolve(process.argv[1])]);
  }
} else {
  app.setAsDefaultProtocolClient('liveplay');
}

// For Windows, we need to handle the protocol differently
const gotTheLock = app.requestSingleInstanceLock();

if (!gotTheLock) {
  app.quit();
} else {
  app.on('second-instance', (event, commandLine) => {
    // Someone tried to run a second instance, we should focus our window
    if (mainWindow) {
      if (mainWindow.isMinimized()) mainWindow.restore();
      mainWindow.focus();
    }
    // On Windows/Linux a double-clicked file while we're already running
    // arrives as an argument on the second instance's command line. Pick it
    // up and open it (or stash it if the window isn't ready yet).
    const f = getOpenableFileFromArgv(commandLine);
    if (f) {
      if (mainWindow && mainWindow.webContents) {
        openFile(f);
      } else {
        fileToOpen = f;
      }
    }
  });

  app.whenReady().then(async () => {
    // Setup bundled ffmpeg before creating window
    const ffmpegReady = await checkAndSetupFfmpeg();
    if (!ffmpegReady) {
      console.error('Warning: Bundled ffmpeg failed to initialize. Audio processing may be limited.');
    }

    // At boot we ONLY adopt an already-running server (via the lockfile).
    // Spawning a fresh server is deferred until the user explicitly picks
    // Local mode in the welcome screen — otherwise we'd briefly spawn a
    // server even for users who only ever use Remote mode.
    void tryReattachLiveplayServer();

    // Best-effort firewall rules so LAN discovery's inbound UDP isn't dropped
    // (no-op on non-Windows / non-elevated; the installer is authoritative).
    try { ensureFirewallRules(); } catch (e) { console.warn('[liveplay-firewall]', e); }

    // Start listening for discovery beacons early so the welcome screen's
    // picker is already populated by the time the user reaches it.
    try { startDiscoveryListener(); } catch (e) { console.warn('[liveplay-discovery]', e); }

    createWindow();

    // NOTE: a file opened before the app was ready (cold start) is delivered
    // via the pull path — the renderer calls `get-pending-open-file` on mount
    // and drives the open itself. We intentionally do NOT push here so a cold
    // start can't double-open (push + pull) the same file.
  });
}

// Handle file opening on macOS (the only platform that fires 'open-file';
// Windows/Linux deliver the path via argv / second-instance instead).
app.on('open-file', (event, filePath) => {
  event.preventDefault();

  if (mainWindow && mainWindow.webContents) {
    // Window is ready, push the file to the renderer immediately.
    openFile(filePath);
  } else {
    // Window not ready yet (cold start) — queue for the renderer's pull.
    fileToOpen = filePath;
  }
});

// Find the first openable project file (.liveplay / .lpa) in a command-line
// argument vector. Used for cold start (process.argv) and Windows/Linux
// warm start (second-instance commandLine).
function getOpenableFileFromArgv(argv) {
  if (!Array.isArray(argv)) return null;
  return argv.find(arg => typeof arg === 'string' &&
    (/\.liveplay$/i.test(arg) || /\.lpa$/i.test(arg))) || null;
}

// Classify a path by extension into the kind the renderer expects.
function fileKindFor(filePath) {
  return /\.lpa$/i.test(filePath) ? 'lpa' : 'liveplay';
}

// Handle command line arguments (Windows/Linux)
if (process.platform === 'win32' || process.platform === 'linux') {
  // Check if a file was passed as argument
  const fileArg = getOpenableFileFromArgv(process.argv);
  if (fileArg) {
    fileToOpen = fileArg;
  }
}

// Pull path for cold start: the renderer asks for any file that was queued
// before it was ready, then drives the open/import flow itself. Returns null
// when nothing is pending. Clears the queue so it's delivered exactly once.
ipcMain.handle('get-pending-open-file', async () => {
  if (!fileToOpen) return null;
  const filePath = fileToOpen;
  fileToOpen = null;
  return { filePath, kind: fileKindFor(filePath) };
});

// Push a queued/warm-start file to the renderer. The renderer (WelcomeScreen)
// owns the actual work: starting/connecting a server and opening or importing
// the project. The main process only hands over the path + kind.
function openFile(filePath) {
  if (!mainWindow || !mainWindow.webContents) return;
  mainWindow.webContents.send('open-file-association', {
    filePath,
    kind: fileKindFor(filePath),
  });
  console.log('Dispatched file association to renderer:', filePath);
}

// MIDI Config Handlers
const midiConfigPath = path.join(app.getPath('userData'), 'midi-config.json');

ipcMain.handle('read-midi-config', async () => {
  try {
    if (fs.existsSync(midiConfigPath)) {
      const data = fs.readFileSync(midiConfigPath, 'utf-8');
      return JSON.parse(data);
    }
    return {};
  } catch (error) {
    console.error('Failed to read MIDI config:', error);
    return {};
  }
});

ipcMain.handle('write-midi-config', async (event, config) => {
  try {
    fs.writeFileSync(midiConfigPath, JSON.stringify(config, null, 2), 'utf-8');
    return { success: true };
  } catch (error) {
    console.error('Failed to write MIDI config:', error);
    throw new Error('Failed to save MIDI configuration');
  }
});

app.on('window-all-closed', () => {
  if (apiServer) {
    apiServer.close();
  }
  // The liveplay audio server is intentionally NOT stopped here — it was
  // spawned detached so it survives renderer reloads and Electron quits.
  // Users explicitly shut it down via the `liveplay-server:shutdown` IPC
  // (or by killing the PID directly). On next launch we reattach via the
  // lockfile.
  if (process.platform !== 'darwin') {
    app.quit();
  }
});

app.on('activate', () => {
  if (mainWindow === null) {
    createWindow();
  }
});

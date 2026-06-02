#!/usr/bin/env node
// =============================================================================
// scripts/build-all.js
// -----------------------------------------------------------------------------
// Unified root build:
//   1. Build the C++ server (via build-server.js)
//   2. On macOS, wrap it as LivePlay Server.app (used by the DMG)
//   3. Build the Nuxt client and produce the Electron installers
//   4. Copy installer artefacts into the repo-root /build/ directory
//
// Invoked by `npm run build` and `npm run build:clean` from the monorepo root.
// =============================================================================
const fs   = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

const REPO_ROOT    = path.resolve(__dirname, '..');
const CLIENT_DIR   = path.join(REPO_ROOT, 'client');
const DIST_ELECTRON = path.join(CLIENT_DIR, 'dist-electron');
const ROOT_BUILD   = path.join(REPO_ROOT, 'build');

function run(cmd, args, opts = {}) {
  console.log(`\n> ${cmd} ${args.join(' ')}`);
  const res = spawnSync(cmd, args, { stdio: 'inherit', shell: process.platform === 'win32', ...opts });
  if (res.status !== 0) {
    console.error(`\n[build-all] Command failed: ${cmd} ${args.join(' ')}`);
    process.exit(res.status ?? 1);
  }
}

// 1. C++ server -------------------------------------------------------------
run('node', [path.join('scripts', 'build-server.js')], { cwd: REPO_ROOT });

// 2. macOS: wrap server binary into a .app bundle ---------------------------
if (process.platform === 'darwin') {
  run('node', [path.join('scripts', 'build-server-app-mac.js')], { cwd: REPO_ROOT });
}

// 3. Client (Nuxt generate + electron-builder) -----------------------------
run('npm', ['run', 'build:electron', '--workspace=client'], { cwd: REPO_ROOT });

// 4. Collect installers into /build/ at the repo root ----------------------
fs.mkdirSync(ROOT_BUILD, { recursive: true });

// Only copy actual installer artefacts — not the unpacked app trees or the
// builder debug/effective-config files. Patterns are intentionally narrow.
const INSTALLER_PATTERNS = [
  /\.exe$/i,        // Windows NSIS installer
  /\.dmg$/i,        // macOS disk image
  /\.pkg$/i,        // macOS installer package (if ever enabled)
  /\.AppImage$/i,   // Linux portable
  /\.deb$/i,        // Debian / Ubuntu
  /\.rpm$/i,        // Fedora / RHEL
  /\.snap$/i,       // Snap (if ever enabled)
];

if (!fs.existsSync(DIST_ELECTRON)) {
  console.error(`[build-all] Expected client output not found at ${DIST_ELECTRON}`);
  process.exit(1);
}

const entries = fs.readdirSync(DIST_ELECTRON, { withFileTypes: true });
let copied = 0;
for (const entry of entries) {
  if (!entry.isFile()) continue;
  if (!INSTALLER_PATTERNS.some(rx => rx.test(entry.name))) continue;
  const src  = path.join(DIST_ELECTRON, entry.name);
  const dest = path.join(ROOT_BUILD, entry.name);
  fs.copyFileSync(src, dest);
  console.log(`[build-all] copied -> ${path.relative(REPO_ROOT, dest)}`);
  copied++;
}

if (copied === 0) {
  console.warn('[build-all] WARNING: no installer artefacts found to copy.');
} else {
  console.log(`\n[build-all] Done. ${copied} installer(s) in ${path.relative(REPO_ROOT, ROOT_BUILD)}/`);
}

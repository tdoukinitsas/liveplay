#!/usr/bin/env node
// =============================================================================
// scripts/build-clean.js
// -----------------------------------------------------------------------------
// Cleans build outputs, then runs the full root build. Keeps vcpkg_installed
// intact so we don't re-download / re-compile all C++ dependencies.
// =============================================================================
const fs   = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

const REPO_ROOT = path.resolve(__dirname, '..');

// Wipe build outputs but preserve server/build/vcpkg_installed so we keep
// the (slow) compiled vcpkg packages.
const SERVER_BUILD = path.join(REPO_ROOT, 'server', 'build');
const VCPKG_INSTALLED = path.join(SERVER_BUILD, 'vcpkg_installed');

function rmrf(p) {
  try { fs.rmSync(p, { recursive: true, force: true }); }
  catch (e) { /* best effort */ }
}

// 1. Wipe client outputs
for (const rel of ['client/.nuxt', 'client/.output', 'client/dist', 'client/dist-electron', 'build']) {
  rmrf(path.join(REPO_ROOT, rel));
}

// 2. Wipe server/build but preserve vcpkg_installed.
if (fs.existsSync(SERVER_BUILD)) {
  const hasVcpkg = fs.existsSync(VCPKG_INSTALLED);
  let tempVcpkg = null;
  if (hasVcpkg) {
    tempVcpkg = path.join(REPO_ROOT, 'server', '.vcpkg_installed_keep');
    rmrf(tempVcpkg);
    fs.renameSync(VCPKG_INSTALLED, tempVcpkg);
  }
  rmrf(SERVER_BUILD);
  if (tempVcpkg) {
    fs.mkdirSync(SERVER_BUILD, { recursive: true });
    fs.renameSync(tempVcpkg, VCPKG_INSTALLED);
  }
}

console.log('[build-clean] Cleaned. Running full build...');

const res = spawnSync('node', [path.join('scripts', 'build-all.js')], {
  cwd: REPO_ROOT,
  stdio: 'inherit',
  shell: process.platform === 'win32',
});
process.exit(res.status ?? 1);

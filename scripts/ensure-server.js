#!/usr/bin/env node
// =============================================================================
// scripts/ensure-server.js
// -----------------------------------------------------------------------------
// Run before `nuxt dev` / electron at the monorepo root. Verifies the C++
// audio server binary exists; if it doesn't, configures + builds it. Designed
// to be a no-op when the binary is already built (fast dev-loop iteration).
// Cross-platform — no shell-specific syntax.
// =============================================================================
const fs        = require('node:fs');
const path      = require('node:path');
const { spawnSync } = require('node:child_process');

const REPO_ROOT  = path.resolve(__dirname, '..');
const SERVER_DIR = path.join(REPO_ROOT, 'server');
const BUILD_DIR  = path.join(SERVER_DIR, 'build');
const EXE_NAME   = process.platform === 'win32' ? 'liveplay-server.exe' : 'liveplay-server';

// Candidate output locations across single-config and multi-config generators.
const BIN_CANDIDATES = [
  path.join(BUILD_DIR, 'Release', EXE_NAME),
  path.join(BUILD_DIR, EXE_NAME),
];

function findBinary() {
  return BIN_CANDIDATES.find(p => fs.existsSync(p));
}

function run(cmd, args, opts = {}) {
  console.log(`> ${cmd} ${args.join(' ')}`);
  const res = spawnSync(cmd, args, { stdio: 'inherit', shell: false, ...opts });
  if (res.status !== 0) {
    process.exitCode = res.status ?? 1;
    process.exit(process.exitCode);
  }
}

function configure() {
  // Pick a preset that matches the host. Falls through to 'default' (Ninja)
  // on Unix; vs2022 on Windows so users don't need Ninja on PATH.
  const preset = process.platform === 'win32' ? 'vs2022' : 'default';
  run('cmake', ['--preset', preset, '-S', SERVER_DIR]);
}

function build() {
  const preset = process.platform === 'win32' ? 'vs2022' : 'default';
  run('cmake', ['--build', BUILD_DIR, '--preset', preset]);
}

const existing = findBinary();
if (existing) {
  console.log(`[liveplay] server binary present: ${existing}`);
  process.exit(0);
}

console.log('[liveplay] server binary not found — building once. Subsequent dev runs skip this step.');

// Configure step is idempotent; skip if CMakeCache.txt is already there.
const cmakeCache = path.join(BUILD_DIR, 'CMakeCache.txt');
if (!fs.existsSync(cmakeCache)) configure();
build();

const built = findBinary();
if (!built) {
  console.error('[liveplay] build finished but binary still missing under', BUILD_DIR);
  process.exit(1);
}
console.log(`[liveplay] built ${built}`);

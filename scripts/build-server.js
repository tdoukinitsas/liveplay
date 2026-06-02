#!/usr/bin/env node
// =============================================================================
// scripts/build-server.js
// -----------------------------------------------------------------------------
// Always configures (idempotent) and builds the C++ server using whichever
// CMake preset fits the host. Used by `npm run server:build`, `npm run build`,
// and CI.
// =============================================================================
const fs        = require('node:fs');
const path      = require('node:path');
const { spawnSync } = require('node:child_process');

const REPO_ROOT  = path.resolve(__dirname, '..');
const SERVER_DIR = path.join(REPO_ROOT, 'server');
const BUILD_DIR  = path.join(SERVER_DIR, 'build');
const PRESET     = process.platform === 'win32' ? 'vs2022' : 'default';

function run(cmd, args, opts = {}) {
  console.log(`> ${cmd} ${args.join(' ')}`);
  const res = spawnSync(cmd, args, { stdio: 'inherit', shell: false, ...opts });
  if (res.status !== 0) process.exit(res.status ?? 1);
}

const cmakeCache = path.join(BUILD_DIR, 'CMakeCache.txt');
if (!fs.existsSync(cmakeCache)) {
  run('cmake', ['--preset', PRESET], { cwd: SERVER_DIR });
}
run('cmake', ['--build', BUILD_DIR, '--preset', PRESET], { cwd: SERVER_DIR });

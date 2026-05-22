#!/usr/bin/env node
// =============================================================================
// scripts/run-server.js
// -----------------------------------------------------------------------------
// Locate the built liveplay-server binary and exec it, forwarding stdio and
// any CLI args. Used by `npm run server:run` and by `npm run dev:all`.
// =============================================================================
const fs        = require('node:fs');
const path      = require('node:path');
const { spawn } = require('node:child_process');

const REPO_ROOT  = path.resolve(__dirname, '..');
const BUILD_DIR  = path.join(REPO_ROOT, 'server', 'build');
const EXE_NAME   = process.platform === 'win32' ? 'liveplay-server.exe' : 'liveplay-server';

const candidates = [
  path.join(BUILD_DIR, 'Release', EXE_NAME),
  path.join(BUILD_DIR, EXE_NAME),
];
const bin = candidates.find(p => fs.existsSync(p));
if (!bin) {
  console.error(
    '[liveplay] server binary not found. Build it first:\n' +
    '   npm run server:build'
  );
  process.exit(1);
}

const child = spawn(bin, process.argv.slice(2), {
  cwd: path.dirname(bin),
  stdio: 'inherit',
});

const forward = (sig) => { try { child.kill(sig); } catch {} };
process.on('SIGINT',  () => forward('SIGINT'));
process.on('SIGTERM', () => forward('SIGTERM'));

child.on('exit', (code, signal) => {
  if (signal) process.exit(0);
  process.exit(code ?? 0);
});

// =====================================================================
// audio-formats-tests.js
// ---------------------------------------------------------------------
// usage:  node client/tests/audio-formats-tests.js        (from the repo root)
//
// The formats LivePlay can play are listed three times, because three
// programs need them and none can import from the others (audit F-43):
//
//   * the server, which refuses anything else on import
//     (audio_extensions() in server/src/net/control_server.cpp);
//   * the renderer, which says so before uploading
//     (PLAYABLE_AUDIO_EXTENSIONS in client/app/utils/audioFormats.ts);
//   * Electron's Open dialog (the 'Audio Files' filter in client/electron/main.js).
//
// They must say the same thing, or a file the dialog offers is refused, or
// one the server would play is greyed out. This reads all three from the
// shipped text and fails if they differ.
// =====================================================================
'use strict';
const fs = require('fs');
const path = require('path');

const root = path.resolve(__dirname, '..', '..');
const read = (rel) => fs.readFileSync(path.join(root, rel), 'utf8');

const failures = [];
const listFrom = (label, text, re) => {
  const m = text.match(re);
  if (!m) {
    failures.push(`${label}: list not found (pattern ${re})`);
    return null;
  }
  return m[1].split(',').map(s => s.trim().replace(/^["'.]+|["']+$/g, '').replace(/^\./, ''))
    .filter(Boolean).sort();
};

const server = listFrom('server', read('server/src/net/control_server.cpp'),
  /audio_extensions\(\)\s*\{[\s\S]*?exts\s*\{([^}]*)\}/);
const client = listFrom('client', read('client/app/utils/audioFormats.ts'),
  /PLAYABLE_AUDIO_EXTENSIONS\s*=\s*\[([^\]]*)\]/);
const dialog = listFrom('dialog', read('client/electron/main.js'),
  /name:\s*'Audio Files',\s*extensions:\s*\[([^\]]*)\]/);

if (server && client && dialog) {
  const same = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);
  if (!same(server, client)) failures.push(`server [${server}] vs client [${client}]`);
  if (!same(server, dialog)) failures.push(`server [${server}] vs Electron dialog [${dialog}]`);
}

if (failures.length) {
  for (const f of failures) console.log(`FAIL  ${f}`);
  process.exit(1);
}
console.log(`PASS  every list says: ${server.join(', ')}`);

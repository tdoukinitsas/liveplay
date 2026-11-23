// Which project file, if any, a command line is asking LivePlay to open.
//
// Kept out of main.js so it can be tested without starting Electron. The shapes
// it has to cope with depend on who launched us:
//   * Windows and KDE pass a plain path;
//   * GNOME honours the desktop entry's %U and passes a file:// URI, with any
//     spaces percent-encoded;
//   * a terminal can pass a path relative to wherever it was run from.
const path = require('path');
const { fileURLToPath } = require('url');

const PROJECT_FILE = /\.(liveplay|lpa)$/i;

function getOpenableFileFromArgv(argv, cwd = process.cwd()) {
  if (!Array.isArray(argv)) return null;
  for (const arg of argv) {
    if (typeof arg !== 'string' || arg.startsWith('-')) continue;
    let candidate = arg;
    if (/^file:\/\//i.test(arg)) {
      try { candidate = fileURLToPath(arg); } catch { continue; }
    }
    if (PROJECT_FILE.test(candidate)) return path.resolve(cwd, candidate);
  }
  return null;
}

// The kind of open flow the renderer should run for a path.
function fileKindFor(filePath) {
  return /\.lpa$/i.test(filePath) ? 'lpa' : 'liveplay';
}

module.exports = { getOpenableFileFromArgv, fileKindFor };

// usage:  node client/tests/open-file-args-tests.js        (from the repo root)
//
// Opening a project from the file manager (#83): the path arrives in a
// different shape depending on the desktop, and a wrong read means the app
// starts and silently opens nothing.
const path = require('path');
const { pathToFileURL } = require('url');
const { getOpenableFileFromArgv, fileKindFor } = require('../electron/open-file-args');

let failures = 0;
const check = (name, actual, expected) => {
  const pass = actual === expected;
  console.log(`${pass ? 'PASS' : 'FAIL'}  ${name}${pass ? '' : `   got ${actual}, wanted ${expected}`}`);
  if (!pass) failures++;
};

const exe = process.execPath;
const cwd = path.resolve('/shows');
const show = path.join(cwd, 'My Show.liveplay');

check('a plain absolute path', getOpenableFileFromArgv([exe, show], cwd), show);
check('an archive', getOpenableFileFromArgv([exe, path.join(cwd, 'a.lpa')], cwd), path.join(cwd, 'a.lpa'));
check('upper-case extension', getOpenableFileFromArgv([exe, path.join(cwd, 'A.LIVEPLAY')], cwd), path.join(cwd, 'A.LIVEPLAY'));
check('a file:// URI with encoded spaces (GNOME %U)',
      getOpenableFileFromArgv([exe, pathToFileURL(show).href], cwd), show);
check('a path relative to the launching terminal',
      getOpenableFileFromArgv([exe, 'My Show.liveplay'], cwd), show);
check('a flag is not a file', getOpenableFileFromArgv([exe, '--config=x.liveplay'], cwd), null);
check('flags before the file are skipped',
      getOpenableFileFromArgv([exe, '--no-sandbox', show], cwd), show);
check('a malformed file URI is skipped', getOpenableFileFromArgv([exe, 'file://host%/x.liveplay'], cwd), null);
check('an unrelated file is ignored', getOpenableFileFromArgv([exe, path.join(cwd, 'a.wav')], cwd), null);
check('no arguments', getOpenableFileFromArgv([exe], cwd), null);
check('not an array', getOpenableFileFromArgv(undefined, cwd), null);
check('.lpa routes to import', fileKindFor('x.LPA'), 'lpa');
check('.liveplay routes to open', fileKindFor('x.liveplay'), 'liveplay');

console.log(failures ? `\n${failures} check(s) failed` : '\nAll checks passed');
process.exit(failures ? 1 : 0);

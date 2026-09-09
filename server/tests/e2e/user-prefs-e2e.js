// What belongs to the person rather than the show (U4).
//
// Four values left the .liveplay document: theme, playbackKeys,
// settings.meterMode and settings.uiScrollToPlaying. A document is a thing you
// mail to a colleague, and every one of those travelled with it — opening
// someone else's show changed your colours and silently reassigned the keys
// your hands already knew.
//
// What this pins:
//   • THE MIGRATION IS NOT LOSSY. A 2.4 document's theme and keymap are read
//     once, at first sign-in, into that person's profile. This is the whole
//     risk of the unit: the values are dropped from the file on the next save,
//     so if the seed does not happen they are simply gone.
//   • ...and it runs at the right MOMENT. A client reads its preferences as
//     soon as its socket comes up, which on the ordinary startup order is
//     before any project is open. Seeding an empty profile then would spend
//     the one chance this person had. So an empty seed writes nothing.
//   • ...and it never runs twice. A second read must not re-import a document
//     over preferences the operator has since changed.
//   • The document stops carrying them. save() drops all four, and the load
//     reports what it found through the existing project_migrated banner, so
//     the file changing shape is something the operator is told about.
//   • settings.meterMode and settings.uiScrollToPlaying are now REFUSED by the
//     settings registry — ignored, not fatal, so a 2.4 client still works.
//   • PATCH /api/project/theme is GONE. It would have answered 200 and lost
//     the value at the next save.
//   • A profile is the CALLER'S. No route names a user id, and an admin gets
//     their own profile from /api/prefs, not a way to read somebody else's.
//   • Preferences are per-person, not per-server: two accounts on one rig keep
//     separate themes and separate keymaps.
//   • Sparse. An absent key means "not chosen", and null clears one — needed
//     because "follow the project's output target" and "explicitly LUFS" are
//     different states.
//   • Validation drops what does not belong rather than refusing the patch,
//     and a malformed keybinding costs that binding, not the keymap.
//   • Deleting an account deletes its profile.
//
// Starts its own server (it owns users.json and prefs/ beside the binary, and
// restarts to prove persistence), so it takes the binary rather than a port.
// Anything it finds in place is moved aside and put back.
//
//   node server/tests/e2e/user-prefs-e2e.js [path-to-liveplay-server]
const { spawn } = require('child_process');
const path = require('path');
const fs   = require('fs');
const os   = require('os');

const EXE = process.argv[2] || path.join('server', 'build', 'Release',
  process.platform === 'win32' ? 'liveplay-server.exe' : 'liveplay-server');

const PORT       = 4574;
const EXE_DIR    = path.dirname(path.resolve(EXE));
const USERS_JSON = path.join(EXE_DIR, 'users.json');
const PREFS_DIR  = path.join(EXE_DIR, 'prefs');
const U_BACKUP   = USERS_JSON + '.e2e-backup';
const P_BACKUP   = PREFS_DIR  + '.e2e-backup';

const ADMIN_NAME = 'e2e-prefs-admin';
const ADMIN_PASS = 'correct-horse-battery';
const OP_NAME    = 'e2e-prefs-operator';
const OP_PASS    = 'stapler-in-the-jelly';

// A 2.4-shaped document carrying all four of the relocated values, so the
// migration has something real to find.
const LEGACY_THEME  = { mode: 'light', accentColor: '#00ff88' };
const LEGACY_KEYMAP = {
  'pause-resume': { key: 'k', ctrlKey: false, shiftKey: false, altKey: false },
  'stop-all':     { key: 'x', ctrlKey: true,  shiftKey: false, altKey: false },
};

let failures = 0;
const ok = (n, p, d) => {
  if (d === undefined) d = '(no detail given — fix the assertion)';
  console.log(`${p ? 'PASS' : 'FAIL'}  ${n}   ${d}`);
  if (!p) failures++;
};
const sleep = ms => new Promise(r => setTimeout(r, ms));

// An unreachable server is a FAILED ASSERTION, not a crashed harness — a run
// that aborts on the first bad fetch reports one line instead of the several
// that say what broke.
async function req(p, { method = 'GET', token, body } = {}) {
  const headers = {};
  if (token) headers.Authorization = `Bearer ${token}`;
  if (body !== undefined) headers['Content-Type'] = 'application/json';
  try {
    const r = await fetch(`http://127.0.0.1:${PORT}${p}`, {
      method, headers,
      body: body === undefined ? undefined : JSON.stringify(body),
    });
    const t = await r.text();
    let parsed; try { parsed = JSON.parse(t); } catch { parsed = t; }
    return { status: r.status, body: parsed };
  } catch (e) {
    return { status: 0, body: null, unreachable: String(e.cause || e) };
  }
}

async function waitForHealth(tries = 40) {
  for (let i = 0; i < tries; i++) {
    try { const r = await fetch(`http://127.0.0.1:${PORT}/api/health`); if (r.ok) return true; }
    catch { /* not up yet */ }
    await sleep(250);
  }
  return false;
}

const login = async (name, password) => {
  const r = await req('/api/auth/login', { method: 'POST', body: { name, password } });
  return (r.body && r.body.token) || '';
};

(async () => {
  let proc = null;
  const startServer = () => {
    proc = spawn(EXE, ['--port', String(PORT)], { stdio: 'ignore' });
    return proc;
  };
  const stopServer = async () => {
    if (!proc) return;
    try { proc.kill(); } catch { /* already gone */ }
    proc = null;
    await sleep(1200);   // the port has to be free before the next bind
  };

  // This suite creates accounts and profiles in the developer's own build
  // directory. Putting back what was there is not optional.
  let hadUsers = false, hadPrefs = false;
  try {
    if (fs.existsSync(USERS_JSON)) { fs.renameSync(USERS_JSON, U_BACKUP); hadUsers = true; }
    if (fs.existsSync(PREFS_DIR))  { fs.renameSync(PREFS_DIR,  P_BACKUP); hadPrefs = true; }
  } catch (e) {
    console.log(`FAIL  could not move existing state aside   ${e}`);
    process.exit(1);
  }

  const tmpDir  = fs.mkdtempSync(path.join(os.tmpdir(), 'lp-prefs-'));
  const projectPath = path.join(tmpDir, 'legacy.liveplay');

  let adminToken = '', opToken = '', opId = '';

  try {
    startServer();
    ok('the server starts', await waitForHealth(), `port ${PORT}`);

    // =====================================================================
    // 1. Unauthenticated — there is nobody for a preference to belong to
    // =====================================================================
    let r = await req('/api/prefs');
    ok('with no accounts, GET /api/prefs refuses with 409 rather than inventing a profile',
       r.status === 409, `${r.status} ${JSON.stringify(r.body)}`);
    ok('...and says why, so the client knows to keep them locally',
       typeof (r.body && r.body.error) === 'string' &&
       /no signed-in user/i.test(r.body.error), JSON.stringify(r.body));

    r = await req('/api/prefs', { method: 'PATCH', body: { meterMode: 'dBTP' } });
    ok('...and PATCH refuses the same way, rather than writing a shared profile',
       r.status === 409, `${r.status}`);

    ok('no prefs directory is created for an installation with no accounts',
       !fs.existsSync(PREFS_DIR), PREFS_DIR);

    // =====================================================================
    // 2. A 2.4 document still loads, and is reported as carrying them
    // =====================================================================
    const legacyDoc = {
      name: 'Legacy Show',
      version: '2.0.0',
      busSchema: 3,
      items: [],
      cartItems: [],
      cartOnlyItems: [],
      cartSlotKeys: { 0: { key: 'F5', ctrlKey: false, shiftKey: false, altKey: false } },
      playbackKeys: LEGACY_KEYMAP,
      theme: LEGACY_THEME,
      settings: { meterMode: 'dBTP', uiScrollToPlaying: true, outputTarget: 'ebu-r128' },
      createdAt: new Date().toISOString(),
      lastModified: new Date().toISOString(),
    };
    fs.writeFileSync(projectPath, JSON.stringify(legacyDoc, null, 2));

    r = await req('/api/project/load', { method: 'POST', body: { path: projectPath } });
    ok('a 2.4 document carrying all four relocated values still loads',
       r.status === 200, `${r.status} ${JSON.stringify(r.body && r.body.error)}`);
    ok('...and the load counts them, so the file changing shape is not silent',
       r.body && r.body.migration && r.body.migration.userPrefsMigrated === 4,
       `userPrefsMigrated=${r.body && r.body.migration && r.body.migration.userPrefsMigrated}`);
    ok('...and the header still exposes the legacy theme, which is what the seed reads',
       r.body && r.body.theme && r.body.theme.accentColor === LEGACY_THEME.accentColor,
       `theme=${JSON.stringify(r.body && r.body.theme) || 'absent'}`);

    // =====================================================================
    // 3. The registry refuses the two relocated settings — ignored, not fatal
    // =====================================================================
    r = await req('/api/project/settings', {
      method: 'PATCH',
      body: { meterMode: 'RMS', uiScrollToPlaying: false, outputTarget: 'radio' },
    });
    ok('a settings patch carrying the relocated keys still SUCCEEDS (a 2.4 client works)',
       r.status === 200, `${r.status}`);
    const settings = (r.body && (r.body.settings || r.body)) || {};
    // NOT `=== undefined`. The values the DOCUMENT was loaded with are still
    // sitting in settings — deliberately, because they are the seed a profile
    // reads, and save() is what drops them. What must not happen is the patch
    // landing: the project stopped being the owner, so a write to it is
    // ignored, and the value present is still the one the file arrived with.
    ok('...but the patch to meterMode was IGNORED — the project no longer owns it',
       settings.meterMode !== 'RMS',
       `${JSON.stringify(settings.meterMode)} (the patch asked for "RMS")`);
    ok('...and so was the one to uiScrollToPlaying',
       settings.uiScrollToPlaying !== false,
       `${JSON.stringify(settings.uiScrollToPlaying)} (the patch asked for false)`);
    ok('...while a real project setting in the same patch was applied',
       settings.outputTarget === 'radio', JSON.stringify(settings.outputTarget));

    r = await req('/api/project/theme', { method: 'PATCH', body: { mode: 'dark' } });
    ok('PATCH /api/project/theme is gone rather than silently discarding a theme',
       r.status === 404 || r.status === 405, `${r.status}`);

    // =====================================================================
    // 4. First sign-in seeds the profile from the open project
    // =====================================================================
    r = await req('/api/users', {
      method: 'POST', body: { name: ADMIN_NAME, password: ADMIN_PASS, role: 'admin' },
    });
    ok('an account can be created', r.status === 200, `${r.status}`);
    adminToken = await login(ADMIN_NAME, ADMIN_PASS);
    ok('...and signed in', adminToken.length > 0, `token ${adminToken ? 'issued' : 'MISSING'}`);

    r = await req('/api/prefs', { token: adminToken });
    ok('the first read of a profile SEEDS it from the project that is open',
       r.status === 200 && r.body && r.body.theme &&
       r.body.theme.accentColor === LEGACY_THEME.accentColor &&
       r.body.theme.mode === LEGACY_THEME.mode,
       `theme=${JSON.stringify(r.body && r.body.theme) || 'absent'}`);
    ok('...including the transport keymap, which is the one nobody can afford to lose',
       r.body && r.body.playbackKeys &&
       r.body.playbackKeys['stop-all'] &&
       r.body.playbackKeys['stop-all'].key === 'x' &&
       r.body.playbackKeys['stop-all'].ctrlKey === true,
       `playbackKeys=${JSON.stringify(r.body && r.body.playbackKeys) || 'absent'}`);
    ok('...and the meter unit and scroll-to-playing',
       r.body && r.body.meterMode === 'dBTP' && r.body.uiScrollToPlaying === true,
       `meterMode=${r.body && r.body.meterMode} scroll=${r.body && r.body.uiScrollToPlaying}`);
    ok('...and cartSlotKeys is NOT taken — a cart wall is the show\'s layout, not a preference',
       r.body && r.body.cartSlotKeys === undefined,
       `profile keys: [${Object.keys(r.body || {}).join(', ')}]`);

    ok('a profile file now exists for that user',
       fs.existsSync(PREFS_DIR) && fs.readdirSync(PREFS_DIR).length === 1,
       fs.existsSync(PREFS_DIR) ? fs.readdirSync(PREFS_DIR).join(',') : 'no prefs dir');

    // =====================================================================
    // 5. Seeding happens ONCE, and never over a real choice
    // =====================================================================
    r = await req('/api/prefs', {
      method: 'PATCH', token: adminToken, body: { theme: { mode: 'dark' } },
    });
    ok('a patch merges rather than replacing — setting the mode keeps the accent colour',
       r.status === 200 && r.body.theme && r.body.theme.mode === 'dark' &&
       r.body.theme.accentColor === LEGACY_THEME.accentColor,
       `theme=${JSON.stringify(r.body && r.body.theme) || 'absent'}`);

    r = await req('/api/prefs', { token: adminToken });
    ok('re-reading does NOT re-import the document over what was just chosen',
       r.body && r.body.theme && r.body.theme.mode === 'dark',
       `mode=${r.body && r.body.theme && r.body.theme.mode} (the document says "${LEGACY_THEME.mode}")`);

    // =====================================================================
    // 6. The document stops carrying them
    // =====================================================================
    // Tokens from here on: an account exists now, so authentication is in
    // force for every ordinary route too (U3).
    const savedPath = path.join(tmpDir, 'saved.liveplay');
    r = await req('/api/project/save', {
      method: 'POST', token: adminToken, body: { path: savedPath } });
    ok('the project saves', r.status === 200, `${r.status}`);
    const saved = JSON.parse(fs.readFileSync(savedPath, 'utf8'));
    ok('...and the saved file no longer carries a theme',
       saved.theme === undefined, `theme=${String(saved.theme)}`);
    ok('...nor a transport keymap',
       saved.playbackKeys === undefined, `playbackKeys=${String(saved.playbackKeys)}`);
    ok('...nor meterMode / uiScrollToPlaying in its settings',
       saved.settings && saved.settings.meterMode === undefined &&
       saved.settings.uiScrollToPlaying === undefined,
       JSON.stringify(saved.settings && {
         meterMode: saved.settings.meterMode,
         uiScrollToPlaying: saved.settings.uiScrollToPlaying }));
    ok('...while cartSlotKeys DID survive, because that one is the show\'s',
       saved.cartSlotKeys && saved.cartSlotKeys['0'] &&
       saved.cartSlotKeys['0'].key === 'F5', JSON.stringify(saved.cartSlotKeys));

    r = await req('/api/project/load', {
      method: 'POST', token: adminToken, body: { path: savedPath } });
    ok('reloading the migrated file reports NOTHING left to migrate',
       r.status === 200 &&
       (!r.body.migration || !r.body.migration.userPrefsMigrated),
       `userPrefsMigrated=${r.body && r.body.migration && r.body.migration.userPrefsMigrated}`);

    // =====================================================================
    // 7. Empty seed writes nothing — the ordering trap
    // =====================================================================
    // A second operator signs in while the OPEN project is the migrated one,
    // which carries none of these values. If that read created an empty
    // profile, this person would have permanently spent their one chance to
    // inherit a keymap from a 2.4 file they open later.
    r = await req('/api/users', {
      method: 'POST', token: adminToken,
      body: { name: OP_NAME, password: OP_PASS, role: 'operator' },
    });
    ok('a second, operator-tier account can be created', r.status === 200, `${r.status}`);
    opId = (r.body && r.body.id) || '';
    opToken = await login(OP_NAME, OP_PASS);

    r = await req('/api/prefs', { token: opToken });
    ok('reading a profile with nothing to seed from returns an empty one',
       r.status === 200 && r.body && Object.keys(r.body).length === 0,
       JSON.stringify(r.body));
    ok('...and writes NO file, so the seed can still happen when a 2.4 project opens',
       fs.readdirSync(PREFS_DIR).length === 1,
       fs.readdirSync(PREFS_DIR).join(','));

    // Now open the legacy document and read again — the migration must work
    // at the moment the values actually exist, not only at first sign-in.
    await req('/api/project/load', {
      method: 'POST', token: adminToken, body: { path: projectPath } });
    r = await req('/api/prefs', { token: opToken });
    ok('opening a 2.4 project and re-reading DOES seed, at the moment there is something to seed',
       r.status === 200 && r.body && r.body.playbackKeys &&
       r.body.playbackKeys['pause-resume'] &&
       r.body.playbackKeys['pause-resume'].key === 'k',
       `playbackKeys=${JSON.stringify(r.body && r.body.playbackKeys) || 'absent'}`);

    // =====================================================================
    // 8. A profile is the caller's, and only the caller's
    // =====================================================================
    r = await req('/api/prefs', {
      method: 'PATCH', token: opToken, body: { theme: { accentColor: '#123456' } },
    });
    ok('the operator can set their own accent colour', r.status === 200,
       `${r.status} ${JSON.stringify(r.body && r.body.theme)}`);

    r = await req('/api/prefs', { token: adminToken });
    ok('...and the ADMIN still sees their own, not the operator\'s',
       r.body && r.body.theme && r.body.theme.accentColor === LEGACY_THEME.accentColor,
       `admin accent=${r.body && r.body.theme && r.body.theme.accentColor}`);
    ok('...and their own theme mode, which the operator never touched',
       r.body && r.body.theme && r.body.theme.mode === 'dark',
       `admin mode=${r.body && r.body.theme && r.body.theme.mode}`);

    // There is deliberately no route that names a user. If one is ever added,
    // this assertion is what should start failing.
    r = await req(`/api/prefs/${opId}`, { token: adminToken });
    ok('there is no per-user preferences route for an admin to reach through',
       r.status === 404 || r.status === 405, `${r.status}`);
    r = await req(`/api/users/${opId}/prefs`, { token: adminToken });
    ok('...by either spelling',
       r.status === 404 || r.status === 405, `${r.status}`);

    // =====================================================================
    // 9. Sparse, and validated
    // =====================================================================
    const beforeBadPatch = (await req('/api/prefs', { token: adminToken })).body;
    r = await req('/api/prefs', {
      method: 'PATCH', token: adminToken,
      body: {
        meterMode: 'not-a-unit',            // bad enum
        uiScrollToPlaying: 'yes',           // bad type
        theme: { accentColor: 'javascript:alert(1)' },   // not a colour
        somethingNobodyHasHeardOf: 42,      // unknown key
      },
    });
    ok('a patch of entirely invalid values still succeeds rather than 400ing the client',
       r.status === 200, `${r.status}`);
    // NOT "the keys are absent" — this profile was seeded earlier and has real
    // values in it. The claim is that nothing MOVED: a bad patch is dropped
    // key by key, and dropping must not mean clobbering what was already set.
    ok('...and changed nothing that was already stored',
       r.status === 200 &&
       JSON.stringify(r.body) === JSON.stringify(beforeBadPatch),
       `before ${JSON.stringify(beforeBadPatch)} / after ${JSON.stringify(r.body)}`);
    ok('...and the unknown key was not kept',
       r.body && r.body.somethingNobodyHasHeardOf === undefined,
       `profile keys: [${Object.keys(r.body || {}).join(', ')}]`);

    r = await req('/api/prefs', {
      method: 'PATCH', token: adminToken,
      body: { playbackKeys: {
        'good':  { key: 'g', ctrlKey: false, shiftKey: false, altKey: false },
        'bad':   { ctrlKey: true },        // no key at all
      } },
    });
    ok('one malformed binding costs that binding, not the whole keymap',
       r.status === 200 && r.body.playbackKeys &&
       r.body.playbackKeys['good'] && r.body.playbackKeys['bad'] === undefined,
       JSON.stringify(r.body && r.body.playbackKeys));

    r = await req('/api/prefs', {
      method: 'PATCH', token: adminToken,
      body: { playbackKeys: { 'deliberately-unbound': null } },
    });
    ok('a null BINDING is stored, because "unbound" differs from "never set"',
       r.status === 200 && r.body.playbackKeys &&
       'deliberately-unbound' in r.body.playbackKeys &&
       r.body.playbackKeys['deliberately-unbound'] === null,
       JSON.stringify(r.body && r.body.playbackKeys));

    await req('/api/prefs', { method: 'PATCH', token: adminToken, body: { meterMode: 'LUFS' } });
    r = await req('/api/prefs', { method: 'PATCH', token: adminToken, body: { meterMode: null } });
    ok('a null KEY clears it, so "follow the project again" can actually be said',
       r.status === 200 && r.body.meterMode === undefined,
       `meterMode=${String(r.body && r.body.meterMode)} after setting it to LUFS then null`);

    // =====================================================================
    // 10. Persistence across a restart
    // =====================================================================
    await req('/api/prefs', {
      method: 'PATCH', token: adminToken, body: { meterMode: 'dBTP', uiScrollToPlaying: true },
    });
    await stopServer();
    startServer();
    ok('the server restarts', await waitForHealth(), `port ${PORT}`);

    adminToken = await login(ADMIN_NAME, ADMIN_PASS);
    r = await req('/api/prefs', { token: adminToken });
    ok('a profile survives a restart',
       r.status === 200 && r.body && r.body.meterMode === 'dBTP' &&
       r.body.uiScrollToPlaying === true && r.body.theme.mode === 'dark',
       JSON.stringify(r.body));

    r = await req('/api/prefs', { token: await login(OP_NAME, OP_PASS) });
    ok('...and so does the other operator\'s, separately',
       r.status === 200 && r.body && r.body.theme &&
       r.body.theme.accentColor === '#123456',
       `operator accent=${r.body && r.body.theme && r.body.theme.accentColor}`);

    // =====================================================================
    // 11. Deleting an account deletes what the server knew about them
    // =====================================================================
    const before = fs.readdirSync(PREFS_DIR).length;
    r = await req(`/api/users/${opId}`, { method: 'DELETE', token: adminToken });
    ok('the operator account can be deleted', r.status === 200, `${r.status}`);
    ok('...and their profile goes with it',
       fs.readdirSync(PREFS_DIR).length === before - 1,
       `${before} → ${fs.readdirSync(PREFS_DIR).length}`);

  } catch (e) {
    ok('the suite ran to completion', false, String(e && e.stack || e));
  } finally {
    await stopServer();
    try { fs.rmSync(PREFS_DIR, { recursive: true, force: true }); } catch { /* ignore */ }
    try { fs.rmSync(USERS_JSON, { force: true }); } catch { /* ignore */ }
    try { if (hadUsers) fs.renameSync(U_BACKUP, USERS_JSON); } catch { /* ignore */ }
    try { if (hadPrefs) fs.renameSync(P_BACKUP, PREFS_DIR); } catch { /* ignore */ }
    try { fs.rmSync(tmpDir, { recursive: true, force: true }); } catch { /* ignore */ }
  }

  console.log(failures === 0
    ? '\nAll user-preference assertions passed.'
    : `\n${failures} assertion(s) FAILED.`);
  process.exit(failures === 0 ? 0 : 1);
})();

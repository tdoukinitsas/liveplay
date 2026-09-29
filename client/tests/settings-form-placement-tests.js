// =====================================================================
// settings-form-placement-tests.js  —  the Users pane's inline forms
// ---------------------------------------------------------------------
// usage:  node client/tests/settings-form-placement-tests.js   (from the repo root)
//
// Every form on the Users pane used to be rendered together at the FOOT of the
// pane. That was reasonable while the pane was an account list, and stopped
// being reasonable once it grew a token group, a posture group and a live list
// of connections below them: "Issue a token" opened a form a screen and a half
// down, and the report was "I thought the button was broken".
//
// A form belongs in the same group as the control that opens it. That is a
// claim about DOM ORDER, which is exactly the kind of claim a careful read of
// the template keeps getting wrong — a form can be moved back to the foot in one
// edit and nothing else in the repo would notice. So it is asserted here.
//
// These assertions are about the SHIPPED template text rather than a copy of
// it: a copy would keep passing while the pane was broken. They are deliberately
// about order and not about pixels — where a form sits in the document is what
// regressed, and it is what a static assertion can hold. How it LOOKS once it is
// there still wants eyes on a running window.
// =====================================================================
const fs = require('fs');
const path = require('path');

const PANE = path.join(__dirname, '..', 'app', 'components', 'SettingsPaneUsers.vue');
const whole = fs.readFileSync(PANE, 'utf8');

// The template only. A `v-if` quoted inside the script half would otherwise be
// found as if it were markup.
const cut = whole.indexOf('<script setup');
if (cut < 0) throw new Error('SettingsPaneUsers.vue: no <script setup> — has the file moved?');
const tpl = whole.slice(0, cut);

let failures = 0;
function check(name, ok, detail) {
  if (ok) { console.log(`  ok   ${name}`); return; }
  failures++;
  console.log(`  FAIL ${name}${detail ? `\n         ${detail}` : ''}`);
}

// Index of the nth occurrence of a literal, or -1. Two of these markers appear
// twice on purpose (the pane has two "add an account" forms, one per posture),
// so nth matters.
function at(needle, nth = 1) {
  let i = -1;
  for (let n = 0; n < nth; n++) {
    i = tpl.indexOf(needle, i + 1);
    if (i < 0) return -1;
  }
  return i;
}

const GROUP = 'class="settings-group-title"';

// Every group heading, so "is there a heading between the button and the form"
// can be asked without guessing which one.
const groupTitles = [];
for (let i = tpl.indexOf(GROUP); i >= 0; i = tpl.indexOf(GROUP, i + 1)) groupTitles.push(i);

// ---------------------------------------------------------------------
// The pairs. Each form, and the control that opens it.
// ---------------------------------------------------------------------
const PAIRS = [
  { name: 'turn the login back on', trigger: 'startAuthChange(true)', form: 'v-if="authChange === true"' },
  { name: 'create the first account', trigger: ['showAdd = true', 1], form: ['v-if="showAdd"', 1] },
  { name: 'sign out everywhere', trigger: 'confirmSignOutEverywhere = true', form: 'v-if="confirmSignOutEverywhere"' },
  // The password form's OTHER trigger is the per-account row below it, which is
  // deliberate and checked separately — see the note further down.
  { name: 'change a password', trigger: 'startPasswordChange(server.authUser?.id)', form: 'v-if="passwordFor"' },
  { name: 'add a colleague', trigger: ['showAdd = true', 2], form: ['v-if="showAdd"', 2] },
  { name: 'remove an account', trigger: 'pendingRemove = u', form: 'v-if="pendingRemove"' },
  { name: 'issue a token', trigger: 'showAddToken = true', form: 'v-if="showAddToken"' },
  // The secret is not opened by a control — it is what issuing one produces, so
  // the form it must follow is the issue form.
  { name: 'the issued secret', trigger: 'v-if="showAddToken"', form: 'v-if="issuedToken"' },
  { name: 'rename a token', trigger: 'startTokenRename(k)', form: 'v-if="renameFor"' },
  { name: 'revoke a token', trigger: 'pendingRevoke = k', form: 'v-if="pendingRevoke"' },
  { name: 'turn the login off', trigger: 'startAuthChange(false)', form: 'v-if="authChange === false"' },
];

console.log('\nEach form sits in the group its control is in:');
for (const p of PAIRS) {
  const t = Array.isArray(p.trigger) ? at(...p.trigger) : at(p.trigger);
  const f = Array.isArray(p.form) ? at(...p.form) : at(p.form);

  if (t < 0 || f < 0) {
    check(p.name, false, t < 0 ? `control not found: ${p.trigger}` : `form not found: ${p.form}`);
    continue;
  }
  if (f < t) {
    check(p.name, false, 'the form is rendered BEFORE the control that opens it');
    continue;
  }
  // The real assertion. A group heading between the two means the form landed
  // in a later group than its button — which is the whole bug.
  const crossed = groupTitles.filter((g) => g > t && g < f);
  check(p.name, crossed.length === 0,
        crossed.length ? `${crossed.length} group heading(s) between the control and the form` : '');
}

console.log('\nEvery form announces itself when it opens:');
// v-reveal puts the cursor in the first field and scrolls the least that will
// bring the form into view. A form without it can still open below the fold on
// a short window, which is the same complaint in a smaller room.
const forms = tpl.match(/<div v-if="[^"]+"[^>]*class="settings-inline-form/g) || [];
check('at least one form is present', forms.length >= PAIRS.length,
      `found ${forms.length}, expected ${PAIRS.length} or more`);
const unrevealed = forms.filter((f) => !f.includes('v-reveal'));
check('all of them carry v-reveal', unrevealed.length === 0,
      unrevealed.length ? unrevealed.join('\n         ') : '');

console.log('\nNothing is rendered below the list that reflows every five seconds:');
// The connected list polls, so anything after it moves under the reader. This
// is the assertion that would have caught the original report.
const connected = at("t('clients.title')");
check('the connected list is on the pane', connected > 0);
const below = (tpl.slice(connected).match(/class="settings-inline-form/g) || []).length;
check('no form below it', below === 0, below ? `${below} form(s) after the connected list` : '');
const noticeIdx = at('v-if="notice"');
check('the last action\'s message is above it', noticeIdx > 0 && noticeIdx < connected);

console.log('\nThe one form that can open away from its control says what it is about:');
// An operator has no account list, so the password form has to live outside the
// admin branch — which means an administrator clicking "Change password" on a
// row opens a form ABOVE that row. That is allowed, and the name is the price.
const pwForm = at('v-if="passwordFor"');
const pwEnd = tpl.indexOf('</div>', tpl.indexOf('settings-actions', pwForm));
check('it names the account', tpl.slice(pwForm, pwEnd).includes('passwordForName'));
check('the row button opens that same one form',
      at('startPasswordChange(u.id)') > pwForm,
      'the per-row control is expected to sit below the form it opens');

console.log(failures === 0
  ? '\nAll settings form-placement assertions passed\n'
  : `\n${failures} assertion(s) failed\n`);
process.exit(failures === 0 ? 0 : 1);

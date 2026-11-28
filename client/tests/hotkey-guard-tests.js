// Which keys the window-level shortcuts leave to the control with focus
// (utils/hotkeyGuard.ts), run against the shipped source with a small fake DOM.
//
// Pinned (R2-06, R2-14, R2-15, F-02, F-09, F-11):
//   * Esc reaches Stop All from a dropdown, a checkbox, a slider, a list row
//     and a row button; only typing in a field, or a control that already
//     handled it, keeps it;
//   * Space / Enter reach GO / Play Selected from a closed dropdown, a checkbox
//     or a list row, but not while typing, in a region with its own keys, or
//     on a focused tab or role="button" row (which they activate);
//   * the arrows reach Select ▲/▼ from a closed dropdown (so a stray ↓ can't
//     re-route a cue) but not from a slider or while typing;
//   * Delete and the edit keys stay with a dropdown or a list (F-11).
//
// Usage: node client/tests/hotkey-guard-tests.js
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require('typescript');

const source = fs.readFileSync(path.join(__dirname, '..', 'app', 'utils', 'hotkeyGuard.ts'), 'utf8');

// ---- A fake DOM: enough of Element for closest() over the selectors used ----
class FakeElement {
  constructor(tag, attrs = {}, parent = null) {
    this.tagName = tag.toUpperCase();
    this.attrs = attrs;
    this.parent = parent;
    this.isContentEditable = attrs.contenteditable === 'true' || attrs.contenteditable === '';
    if (tag === 'input') this.type = (attrs.type || 'text').toLowerCase();
  }
  getAttribute(name) { return name in this.attrs ? this.attrs[name] : null; }
  matchesOne(sel) {
    const m = /^([a-z]*)((?:\[[^\]]+\])*)(?::not\(([a-z]+)\))?$/.exec(sel.trim());
    if (!m) throw new Error(`fake DOM can't match selector "${sel}"`);
    const [, tag, attrPart, notTag] = m;
    if (tag && this.tagName !== tag.toUpperCase()) return false;
    if (notTag && this.tagName === notTag.toUpperCase()) return false;
    for (const a of attrPart.match(/\[[^\]]+\]/g) || []) {
      const am = /^\[([a-z-]+)(?:="([^"]*)")?\]$/.exec(a);
      if (!am) throw new Error(`fake DOM can't match attribute "${a}"`);
      const [, name, value] = am;
      const actual = name === 'type' && this.tagName === 'INPUT' && !(name in this.attrs) ? undefined
                   : this.getAttribute(name);
      if (actual === null || actual === undefined) return false;
      if (value !== undefined && actual !== value) return false;
    }
    return true;
  }
  closest(selectorList) {
    const sels = selectorList.split(',');
    for (let el = this; el; el = el.parent) {
      if (sels.some(s => el.matchesOne(s))) return el;
    }
    return null;
  }
}

const body = new FakeElement('body');
const document = { body, activeElement: body };
const context = vm.createContext({ document, Element: FakeElement, exports: {} });
vm.runInContext(ts.transpileModule(source, {
  compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022 },
}).outputText, context);
const { shouldIgnoreHotkey } = context.exports;

const key = (k, el, extra = {}) => {
  document.activeElement = el;
  return shouldIgnoreHotkey({ key: k, target: el, defaultPrevented: false, altKey: false,
                              ctrlKey: false, metaKey: false, ...extra });
};
const reaches = (k, el, extra) => !key(k, el, extra);

// The places focus ends up after a click.
const panel     = new FakeElement('div', {}, body);
const select    = new FakeElement('select', {}, panel);
const checkbox  = new FakeElement('input', { type: 'checkbox' }, panel);
const range     = new FakeElement('input', { type: 'range' }, panel);
const textField = new FakeElement('input', {}, panel);
const number    = new FakeElement('input', { type: 'number' }, panel);
const textarea  = new FakeElement('textarea', {}, panel);
const listbox   = new FakeElement('div', { role: 'listbox' }, panel);
const option    = new FakeElement('div', { role: 'option', tabindex: '0' }, listbox);
const rowButton = new FakeElement('div', { role: 'button', tabindex: '0' }, panel);
const button    = new FakeElement('button', {}, panel);
const tab       = new FakeElement('div', { role: 'tab' }, panel);
const ownKeys   = new FakeElement('div', { 'data-own-keys': '' }, body);
const inOwnKeys = new FakeElement('button', {}, ownKeys);
const menu      = new FakeElement('div', { role: 'menu' }, body);
const menuItem  = new FakeElement('div', { role: 'menuitem' }, menu);

// ---- Esc: Stop All from anywhere but a field (R2-06, R2-14, R2-15) ----------
for (const [name, el] of [['a dropdown', select], ['a checkbox', checkbox], ['a slider', range],
                          ['a list row', option], ['a row button', rowButton], ['a button', button],
                          ['a tab', tab], ['the page', body]]) {
  assert.ok(reaches('Escape', el), `Esc on ${name} must reach Stop All`);
}
assert.ok(!reaches('Escape', textField), 'Esc while typing in a field stays with the field');
assert.ok(!reaches('Escape', textarea), '...and in a text area');
assert.ok(!reaches('Escape', select, { defaultPrevented: true }),
  'Esc a control already handled (a popover closing) is not Stop All (F-02)');

// ---- Space / Enter: GO / Play Selected --------------------------------------
for (const k of [' ', 'Enter']) {
  const what = k === ' ' ? 'Space' : 'Enter';
  assert.ok(reaches(k, select), `${what} on a closed dropdown reaches the transport (R2-06)`);
  assert.ok(reaches(k, checkbox), `${what} on a checkbox reaches the transport`);
  assert.ok(reaches(k, option), `${what} on a list row reaches the transport`);
  assert.ok(reaches(k, button), `${what} on a plain button reaches the transport (as in 2.5.3)`);
  assert.ok(!reaches(k, textField), `${what} while typing stays with the field`);
  assert.ok(!reaches(k, number), `${what} in a number field stays with the field`);
  assert.ok(!reaches(k, inOwnKeys), `${what} in a region with its own keys stays there`);
  assert.ok(!reaches(k, menuItem), `${what} on an open menu stays with it`);
  assert.ok(!reaches(k, tab), `${what} on a focused tab activates the tab (F-09)`);
  assert.ok(!reaches(k, rowButton), `${what} on a focused row button activates the row`);
}

// ---- Arrows: Select ▲/▼ -------------------------------------------------------
assert.ok(reaches('ArrowDown', select), 'ArrowDown on a closed dropdown moves the selection, not its value (R2-06)');
assert.ok(reaches('ArrowUp', checkbox), 'ArrowUp on a checkbox moves the selection');
assert.ok(!reaches('ArrowDown', range), 'the arrows on a slider move the slider');
assert.ok(!reaches('ArrowDown', textField), 'the arrows while typing move the caret');
assert.ok(!reaches('ArrowDown', inOwnKeys), 'the arrows in a region with its own keys stay there');
assert.ok(!reaches('ArrowDown', select, { altKey: true }), 'Alt+ArrowDown still opens a dropdown');

// ---- Other keys: still the focused control's (F-11) ---------------------------
assert.ok(!reaches('Delete', select), 'Delete on a dropdown never deletes the cue');
assert.ok(!reaches('Delete', option), 'Delete in the events list never deletes the cue (F-11)');
assert.ok(!reaches('Backspace', textField), 'Backspace while typing is typing');
assert.ok(!reaches('c', select, { ctrlKey: true }), 'Ctrl+C on a dropdown is the dropdown\'s');
assert.ok(reaches('Delete', body), 'Delete on the page is the workspace\'s');
assert.ok(reaches('q', button), 'a cart key on a plain button fires the cart');

console.log('PASS: Esc, Space, Enter and the arrows reach the transport from dropdowns, rows and boxes; fields, own-key regions, sliders and tabs keep theirs.');

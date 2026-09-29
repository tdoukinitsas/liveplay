// Exercise the shipped send handler and inspect window-independent Settings placement.
// Usage: node client/tests/mixer-regression-tests.js
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require('typescript');
const { parse } = require('@vue/compiler-sfc');
const { baseParse } = require('@vue/compiler-dom');
const read = name => fs.readFileSync(path.join(__dirname, '..', 'app', name), 'utf8');

(async () => {
  const source = parse(read('components/BusSendList.vue')).descriptor.scriptSetup.content;
  const calls = [];
  let rejectPatch = false;
  const server = {
    async patchBus(id, patch) {
      calls.push(['patch', id, patch]);
      if (rejectPatch) throw new Error('409 — {"error":"refused"}');
    },
    async fetchBuses() { calls.push(['refresh']); },
  };
  const context = vm.createContext({
    defineProps: () => ({ bus: { id: 'source', sends: [] }, buses: [] }),
    useLocalization: () => ({ t: key => key }),
    useLiveplayServer: () => server,
    useProject: () => ({ saveProject: () => { calls.push(['save']); return Promise.resolve(true); } }),
    computed: fn => ({ get value() { return fn(); } }),
    ref: value => ({ value }), onBeforeUnmount: () => {},
    setTimeout: () => 1, clearTimeout: () => {},
    exports: {},
  });
  vm.runInContext(ts.transpileModule(source, {
    compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022 },
  }).outputText, context);
  const next = [{ id: 'destination', tap: 'pre', levelDb: -6 }];
  await context.commit(next);
  assert.deepEqual(JSON.parse(JSON.stringify(calls)), [['patch', 'source', { sends: next }], ['save']]);
  calls.length = 0;
  rejectPatch = true;
  await context.commit(next);
  assert.deepEqual(calls.map(c => c[0]), ['patch', 'refresh']);

  const template = parse(read('app.vue')).descriptor.template.content;
  const root = baseParse(template).children.find(node => node.tag === 'div');
  const settings = root.children.filter(node => node.tag === 'SettingsPage');
  assert.equal(settings.length, 1, 'Settings must render outside the window-mode branches');
  assert.equal(settings[0].props.length, 0, 'Settings placement must be unconditional');
  console.log('PASS: successful send edits save, refused edits do not; Settings is available in every window.');
})().catch(error => { console.error(error); process.exitCode = 1; });

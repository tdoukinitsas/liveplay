<template>
  <div class="settings-pane conn-pane">
    <h3 class="settings-pane-title">{{ t('settings.sectionConnections') }}</h3>
    <p class="settings-pane-intro">{{ t('events.connections.intro') }}</p>

    <p v-if="!currentProject" class="settings-help">{{ t('events.connections.noProject') }}</p>
    <p v-else-if="pluginsStatus === 'unsupported'" class="settings-help">{{ t('events.unsupported') }}</p>

    <template v-else>
      <p v-if="saveError" class="conn-error">{{ saveError }}</p>

      <section class="conn-list">
        <p v-if="connections.length === 0" class="settings-help">{{ t('events.connections.empty') }}</p>

        <div
          v-for="c in connections"
          :key="c.id"
          class="conn-row"
          :class="{ 'conn-row--disabled': c.enabled === false, 'conn-row--editing': draft?.id === c.id }"
        >
          <div class="conn-row-head">
            <input
              type="checkbox"
              class="conn-toggle"
              :checked="c.enabled !== false"
              :disabled="saving"
              :title="t('events.connections.enabled')"
              @change="setEnabled(c, ($event.target as HTMLInputElement).checked)"
            />
            <span class="material-symbols-rounded conn-icon">{{ pluginOf(c.plugin)?.icon || 'extension' }}</span>
            <span class="conn-text">
              <span class="conn-name">{{ c.name }}</span>
              <span class="conn-meta">
                {{ pluginOf(c.plugin)?.label ?? t('events.connections.unknownPlugin', { plugin: c.plugin }) }}
                · {{ usage.get(c.id) ? t('events.connections.usage', { count: usage.get(c.id)! }) : t('events.connections.unused') }}
              </span>
            </span>
            <button type="button" class="conn-icon-btn" :title="t('events.connections.edit')"
                    :disabled="saving" @click="startEdit(c)">
              <span class="material-symbols-rounded">edit</span>
            </button>
            <button type="button" class="conn-icon-btn conn-icon-btn--danger" :title="t('events.connections.delete')"
                    :disabled="saving" @click="confirmDeleteId = c.id">
              <span class="material-symbols-rounded">delete</span>
            </button>
          </div>

          <!-- Inline rather than a modal: Settings is already a page over the
               show, and a dialog on top of it would be a third layer. -->
          <div v-if="confirmDeleteId === c.id" class="conn-confirm">
            <span>
              {{ usage.get(c.id)
                ? t('events.connections.deleteInUse', { count: usage.get(c.id)!, name: c.name })
                : t('events.connections.deleteConfirm', { name: c.name }) }}
            </span>
            <Btn icon="delete" :text="t('events.connections.delete')" :disabled="saving" @click="deleteConnection(c)" />
            <Btn icon="close" :text="t('events.connections.cancel')" @click="confirmDeleteId = null" />
          </div>
        </div>
      </section>

      <!-- Add: pick the type first, because the type decides every field. -->
      <section v-if="!draft" class="conn-add">
        <select v-model="addPluginId" class="settings-select conn-add-select" :disabled="connectablePlugins.length === 0">
          <option v-for="p in connectablePlugins" :key="p.id" :value="p.id">{{ p.label }}</option>
        </select>
        <Btn icon="add" :text="t('events.connections.add')" :disabled="!addPluginId" @click="startAdd" />
      </section>
      <p v-if="!draft && pluginsStatus === 'ok' && connectablePlugins.length === 0" class="settings-help">
        {{ t('events.noPlugins') }}
      </p>

      <!-- The editor works on a copy: nothing reaches the server (or the other
           clients) until Save, so a half-typed IP address never fires anything. -->
      <section v-if="draft" class="conn-editor">
        <h4 class="conn-editor-title">
          <span class="material-symbols-rounded">{{ draftPlugin?.icon || 'extension' }}</span>
          {{ isNew
            ? t('events.connections.newTitle', { plugin: draftPlugin?.label ?? draft.plugin })
            : t('events.connections.editTitle', { name: originalName }) }}
        </h4>
        <p v-if="draftPlugin?.description" class="settings-help">{{ draftPlugin.description }}</p>

        <div class="settings-field">
          <label class="settings-label" for="conn-name">{{ t('events.connections.name') }}</label>
          <input id="conn-name" v-model="draft.name" class="settings-input" type="text" />
        </div>

        <label class="settings-label settings-label--checkbox">
          <input type="checkbox" :checked="draft.enabled !== false" @change="draft.enabled = ($event.target as HTMLInputElement).checked" />
          {{ t('events.connections.enabled') }}
        </label>

        <!-- Discovery: pick a device the server can see instead of typing its address. -->
        <div v-if="draftPlugin?.discoverable" class="conn-discover">
          <Btn :icon="discovering ? 'progress_activity' : 'radar'" :text="t('events.connections.discover')"
               :disabled="discovering" @click="runDiscover" />
          <span v-if="discoverMessage" class="settings-help">{{ discoverMessage }}</span>
          <ul v-if="discovered.length" class="conn-discover-list">
            <li v-for="d in discovered" :key="d.label">
              <button type="button" class="conn-discover-item" @click="useDiscovered(d)">
                <span class="material-symbols-rounded">{{ draftPlugin.icon || 'device_hub' }}</span>
                {{ d.label }}
              </button>
            </li>
          </ul>
        </div>

        <SchemaForm
          v-if="draftPlugin && draftPlugin.connectionFields.length > 0"
          :fields="draftPlugin.connectionFields"
          :model-value="draft.config"
          :options-context="{ plugin: draft.plugin, connection: draft }"
          @update:model-value="(v: Record<string, unknown>) => { if (draft) draft.config = v; }"
        />
        <p v-else-if="draftPlugin" class="settings-help">{{ t('events.connections.noFields') }}</p>
        <p v-else class="settings-help">{{ t('events.connections.unknownPlugin', { plugin: draft.plugin }) }}</p>

        <!-- Test against the draft, before saving: the point is to find out
             the address is wrong while you are still looking at it. -->
        <div v-if="draftPlugin && draftPlugin.actions.length > 0" class="conn-test">
          <div class="settings-field">
            <label class="settings-label" for="conn-test-action">{{ t('events.connections.testWith') }}</label>
            <select id="conn-test-action" v-model="testActionId" class="settings-select">
              <option v-for="a in draftPlugin.actions" :key="a.id" :value="a.id">{{ a.label }}</option>
            </select>
          </div>
          <SchemaForm
            v-if="testAction && testAction.fields.length > 0"
            :fields="testAction.fields"
            :model-value="testOptions"
            :options-context="{ plugin: draft.plugin, connection: draft }"
            @update:model-value="(v: Record<string, unknown>) => { testOptions = v; }"
          />
          <div class="conn-test-run">
            <Btn
              :icon="testing ? 'progress_activity' : 'send'"
              :text="testing ? t('events.testing') : t('events.test')"
              :disabled="testing || !testAction"
              @click="runTest"
            />
            <span v-if="testResult" class="conn-result" :class="testResult.ok ? 'ok' : 'error'">
              {{ testResult.ok ? t('events.testOk') : t('events.testFailed', { error: testResult.error || '?' }) }}
            </span>
          </div>
        </div>

        <div class="conn-editor-actions">
          <Btn icon="check" :text="t('events.connections.save')" :disabled="saving || !draft.name.trim()" @click="saveDraft" />
          <Btn icon="close" :text="t('events.connections.cancel')" :disabled="saving" @click="cancelDraft" />
        </div>
      </section>
    </template>
  </div>
</template>

<script setup lang="ts">
// Settings > Connections (#84): the show's event destinations. Project-scoped
// like the Project pane: the list is part of the show file, so it travels
// with the show to the next venue (where only the addresses need changing).
// Every change is a whole-list PUT; the server broadcasts it to every client.
import { v4 as uuidv4 } from 'uuid';
import type { DiscoveredDevice, EventConnection } from '~/types/events';
import { defaultsFor, findAction, findPlugin, fillDefaults } from '~/utils/events';
import Btn from './Btn.vue';
import SchemaForm from './SchemaForm.vue';

const { t } = useLocalization();
const { currentProject } = useProject();
const {
  plugins, pluginsStatus, connections,
  refreshPlugins, saveConnections, testEvent, connectionUsage, discover,
} = useEvents();

onMounted(() => { void refreshPlugins(); });

const pluginOf = (id: string) => findPlugin(plugins.value, id);
const connectablePlugins = computed(() => plugins.value.filter(p => p.needsConnection));
const usage = computed(() => connectionUsage());

const addPluginId = ref('');
watch(connectablePlugins, (list) => {
  if (!list.some(p => p.id === addPluginId.value)) addPluginId.value = list[0]?.id ?? '';
}, { immediate: true });

const saving = ref(false);
const saveError = ref('');
const confirmDeleteId = ref<string | null>(null);

async function commit(list: EventConnection[]): Promise<boolean> {
  saving.value = true;
  saveError.value = '';
  try {
    await saveConnections(list);
    return true;
  } catch (e: any) {
    saveError.value = t('events.connections.saveFailed', { error: e?.message ?? String(e) });
    return false;
  } finally {
    saving.value = false;
  }
}

function setEnabled(c: EventConnection, on: boolean) {
  void commit(connections.value.map(x => (x.id === c.id ? { ...x, enabled: on } : x)));
}

async function deleteConnection(c: EventConnection) {
  const ok = await commit(connections.value.filter(x => x.id !== c.id));
  if (!ok) return;
  confirmDeleteId.value = null;
  if (draft.value?.id === c.id) cancelDraft();
}

// ---- editing ---------------------------------------------------------------
const draft = ref<EventConnection | null>(null);
const isNew = ref(false);
const originalName = ref('');
const draftPlugin = computed(() => (draft.value ? pluginOf(draft.value.plugin) : null));

function startAdd() {
  const plugin = pluginOf(addPluginId.value);
  if (!plugin) return;
  const n = connections.value.filter(c => c.plugin === plugin.id).length + 1;
  draft.value = {
    id: uuidv4(),
    name: t('events.connections.defaultName', { plugin: plugin.label, n }),
    plugin: plugin.id,
    enabled: true,
    config: defaultsFor(plugin.connectionFields),
  };
  isNew.value = true;
  originalName.value = '';
  discovered.value = [];
  discoverMessage.value = '';
  confirmDeleteId.value = null;
  void nextTick(resetTest);
}

// ---- discovery ----------------------------------------------------------------
const discovering = ref(false);
const discovered = ref<DiscoveredDevice[]>([]);
const discoverMessage = ref('');

async function runDiscover() {
  if (!draft.value) return;
  discovering.value = true;
  discoverMessage.value = '';
  discovered.value = [];
  try {
    discovered.value = await discover(draft.value.plugin);
    if (!discovered.value.length) discoverMessage.value = t('events.connections.discoverNone');
  } catch (e: any) {
    discoverMessage.value = t('events.connections.discoverFailed', { error: e?.message ?? String(e) });
  } finally {
    discovering.value = false;
  }
}

// Fill the draft from a found device. The name follows the device only while
// it is still the one we made up; a name the operator typed is theirs.
function useDiscovered(d: DiscoveredDevice) {
  if (!draft.value) return;
  draft.value.config = { ...draft.value.config, ...d.config };
  if (d.name && (isNew.value || !draft.value.name.trim())) draft.value.name = d.name;
  discovered.value = [];
  discoverMessage.value = t('events.connections.discoverUsed', { device: d.label });
}

function startEdit(c: EventConnection) {
  const copy: EventConnection = JSON.parse(JSON.stringify(c));
  // Fields a newer schema added since this connection was made show their
  // defaults rather than blank.
  copy.config = fillDefaults(pluginOf(c.plugin)?.connectionFields, copy.config);
  draft.value = copy;
  isNew.value = false;
  originalName.value = c.name;
  confirmDeleteId.value = null;
  void nextTick(resetTest);
}

function cancelDraft() {
  draft.value = null;
  discovered.value = [];
  discoverMessage.value = '';
  testResult.value = null;
}

async function saveDraft() {
  const d = draft.value;
  if (!d || !d.name.trim()) return;
  const next: EventConnection = { ...JSON.parse(JSON.stringify(d)), name: d.name.trim() };
  const list = connections.value.some(c => c.id === d.id)
    ? connections.value.map(c => (c.id === d.id ? next : c))
    // Another client may have deleted the one being edited meanwhile; saving
    // puts it back rather than losing what was typed.
    : [...connections.value, next];
  if (await commit(list)) cancelDraft();
}

// ---- test ------------------------------------------------------------------
const testActionId = ref('');
const testOptions = ref<Record<string, unknown>>({});
const testing = ref(false);
const testResult = ref<{ ok: boolean; error?: string } | null>(null);
const testAction = computed(() => findAction(draftPlugin.value, testActionId.value));

watch(draftPlugin, (p) => {
  if (!p) return;
  // A test-only action (a harmless check) first: testing a connection must
  // not recall a snapshot or move a fader because it happens to be listed first.
  if (!p.actions.some(a => a.id === testActionId.value))
    testActionId.value = (p.actions.find(a => a.testOnly) ?? p.actions[0])?.id ?? '';
}, { immediate: true });
// A fresh draft (or another action) starts the test form from its defaults;
// the watch alone would miss a new draft that lands on the same action.
function resetTest() {
  testOptions.value = defaultsFor(testAction.value?.fields);
  testResult.value = null;
}
watch(testAction, resetTest, { immediate: true });

async function runTest() {
  const d = draft.value;
  if (!d || !testAction.value) return;
  testing.value = true;
  testResult.value = null;
  try {
    testResult.value = await testEvent(
      { plugin: d.plugin, action: testAction.value.id, options: testOptions.value, connectionId: d.id },
      { connection: d },
    );
  } finally {
    testing.value = false;
  }
}
</script>

<style scoped>
.conn-discover { display: flex; flex-wrap: wrap; align-items: center; gap: var(--spacing-sm); margin: var(--spacing-sm) 0; }
.conn-discover-list { list-style: none; margin: 0; padding: 0; width: 100%; display: flex; flex-direction: column; gap: 4px; }
.conn-discover-item {
  display: flex; align-items: center; gap: 6px; width: 100%; text-align: left;
  padding: 6px 10px; border: 1px solid var(--color-border); border-radius: 6px;
  background: var(--color-surface); color: var(--color-text); cursor: pointer;
}
.conn-discover-item:hover { background: var(--color-surface-hover); }
.conn-discover-item .material-symbols-rounded { font-size: 18px; }

.conn-error {
  margin: 0;
  font-size: 13px;
  color: #e53e3e;
}

.conn-list {
  display: flex;
  flex-direction: column;
  border: 1px solid var(--color-border);
  border-radius: 6px;
  background: var(--color-surface);
  overflow: hidden;

  &:has(> .settings-help:only-child) {
    padding: 12px;
  }
}

.conn-row {
  display: flex;
  flex-direction: column;
  border-bottom: 1px solid var(--color-border);

  &:last-child {
    border-bottom: none;
  }
}

.conn-row--editing {
  box-shadow: inset 3px 0 0 var(--color-accent);
}

.conn-row--disabled .conn-text,
.conn-row--disabled .conn-icon {
  opacity: 0.5;
}

.conn-row-head {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 8px 12px;
}

.conn-toggle {
  width: 16px;
  height: 16px;
  accent-color: var(--color-accent);
  cursor: pointer;
}

.conn-icon {
  font-size: 20px;
  color: var(--color-text-secondary);
}

.conn-text {
  flex: 1;
  min-width: 0;
  display: flex;
  flex-direction: column;
}

.conn-name {
  font-size: 14px;
  color: var(--color-text-primary);
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.conn-meta {
  font-size: 12px;
  color: var(--color-text-secondary);
}

.conn-icon-btn {
  display: flex;
  align-items: center;
  justify-content: center;
  width: 30px;
  height: 30px;
  background: none;
  border: 1px solid transparent;
  border-radius: 6px;
  color: var(--color-text-secondary);
  cursor: pointer;

  .material-symbols-rounded {
    font-size: 18px;
  }

  &:hover:not(:disabled) {
    color: var(--color-text-primary);
    background: var(--color-surface-hover);
  }

  &:disabled {
    opacity: 0.4;
    cursor: not-allowed;
  }
}

.conn-icon-btn--danger:hover:not(:disabled) {
  color: var(--color-danger, #e53e3e);
}

.conn-confirm {
  display: flex;
  align-items: center;
  flex-wrap: wrap;
  gap: 8px;
  padding: 8px 12px 10px 38px;
  font-size: 13px;
  color: var(--color-text-primary);

  > span {
    flex: 1 1 240px;
  }
}

.conn-add {
  display: flex;
  gap: 8px;
  align-items: center;
}

.conn-add-select {
  flex: 0 1 240px;
}

.conn-editor {
  display: flex;
  flex-direction: column;
  gap: 14px;
  padding: 16px;
  border: 1px solid var(--color-border);
  border-radius: 6px;
  background: var(--color-surface);
}

.conn-editor-title {
  display: flex;
  align-items: center;
  gap: 8px;
  margin: 0;
  font-size: 15px;
  font-weight: 600;
  color: var(--color-text-primary);

  .material-symbols-rounded {
    font-size: 20px;
  }
}

.conn-test {
  display: flex;
  flex-direction: column;
  gap: 10px;
  padding-top: 12px;
  border-top: 1px solid var(--color-border);
}

.conn-test-run {
  display: flex;
  align-items: center;
  gap: 10px;
}

.conn-result {
  font-size: 13px;

  &.ok { color: var(--color-success); }
  &.error { color: #e53e3e; }
}

.conn-editor-actions {
  display: flex;
  gap: 8px;
}
</style>

<template>
  <!-- Note: NOT inside <Teleport> — Vue scoped styles don't reach teleported
       nodes, which would leave the modal unstyled in production builds
       (same reasoning as ProjectSettingsModal.vue). -->
  <div v-if="open" class="outmap-backdrop" @click.self="requestClose">
    <div class="outmap-modal">
      <header class="modal-header">
        <h2>{{ t('mixer.outputMapTitle') }}</h2>
        <button class="close-x" @click="requestClose">✕</button>
      </header>

      <div class="modal-body">
        <p class="outmap-intro">{{ t('mixer.outputMapIntro') }}</p>

        <p v-if="loadError" class="outmap-error">{{ loadError }}</p>

        <!-- ================= Logical outputs ================= -->
        <section class="outmap-section">
          <div v-if="rows.length === 0" class="outmap-empty">
            {{ t('mixer.outputMapEmpty') }}
          </div>

          <div v-for="(row, ri) in rows" :key="row.key" class="outmap-row">
            <div class="outmap-rowhead">
              <input
                class="outmap-name"
                :value="row.name"
                :placeholder="t('mixer.outputMapOutputName')"
                :readonly="row.builtin"
                :title="row.builtin ? t('mixer.outputMapBuiltin') : ''"
                @input="row.name = ($event.target as HTMLInputElement).value"
              />
              <!-- A built-in row (D26) is a name the server special-cases
                   when it has no mapping; it cannot be deleted, only left
                   unmapped, so the delete button gives way to a hint. -->
              <span
                v-if="row.builtin"
                class="outmap-builtin"
                :title="t('mixer.outputMapBuiltin')"
              >
                <span class="material-symbols-rounded">lock</span>
              </span>
              <button
                v-else
                class="outmap-iconbtn outmap-iconbtn--danger"
                :title="t('mixer.outputMapDeleteOutput')"
                @click="requestDeleteOutput(ri)"
              >
                <span class="material-symbols-rounded">delete</span>
              </button>
            </div>

            <div class="outmap-channels">
              <div v-for="(ch, ci) in row.channels" :key="ci" class="outmap-channel">
                <select
                  class="outmap-select"
                  :value="ch.device"
                  @change="ch.device = ($event.target as HTMLSelectElement).value"
                >
                  <option value="">{{ t('mixer.outputMapNoneSelected') }}</option>
                  <option
                    v-for="d in deviceOptions(ch.device)"
                    :key="d.id"
                    :value="d.id"
                  >{{ d.display_name }}</option>
                </select>
                <input
                  type="number"
                  class="outmap-chnum"
                  min="0"
                  step="1"
                  :value="ch.hwChannel"
                  :title="t('mixer.outputMapChannel')"
                  @input="ch.hwChannel = Number(($event.target as HTMLInputElement).value) || 0"
                />
                <button
                  class="outmap-iconbtn"
                  :title="t('mixer.outputMapRemoveChannel')"
                  @click="row.channels.splice(ci, 1)"
                >
                  <span class="material-symbols-rounded">close</span>
                </button>
              </div>
              <button class="outmap-addchannel" @click="addChannel(row)">
                <span class="material-symbols-rounded">add</span>
                {{ t('mixer.outputMapAddChannel') }}
              </button>
            </div>
          </div>

          <button class="outmap-addoutput" @click="addOutput">
            <span class="material-symbols-rounded">add</span>
            {{ t('mixer.outputMapAddOutput') }}
          </button>
        </section>

        <!-- ================= Unmapped bus targets ================= -->
        <section class="outmap-section">
          <h3 class="outmap-h3">{{ t('mixer.outputMapUnmappedSection') }}</h3>
          <p v-if="unmappedBusTargets.length === 0" class="outmap-none">
            {{ t('mixer.outputMapUnmappedEmpty') }}
          </p>
          <ul v-else class="outmap-unmapped-list">
            <li v-for="entry in unmappedBusTargets" :key="entry.id" class="outmap-unmapped-item">
              <span class="outmap-unmapped-bus">{{ entry.name }}</span>
              <span class="outmap-unmapped-target">→ {{ entry.target }}</span>
              <button class="outmap-mapnow" @click="mapNow(entry.target)">
                {{ t('mixer.outputMapMapNow') }}
              </button>
            </li>
          </ul>
        </section>

        <!-- ================= Delete confirmation ================= -->
        <div v-if="pendingDelete" class="outmap-confirm">
          <p class="outmap-confirm-title">
            {{ t('mixer.outputMapDeleteConfirmTitle', { name: pendingDelete.name }) }}
          </p>
          <p class="outmap-confirm-body">{{ t('mixer.outputMapDeleteConfirmBody') }}</p>
          <ul class="outmap-confirm-buses">
            <li v-for="n in pendingDelete.affected" :key="n">{{ n }}</li>
          </ul>
          <div class="outmap-confirm-actions">
            <button class="modal-btn" @click="cancelDelete">
              {{ t('mixer.outputMapDeleteConfirmCancel') }}
            </button>
            <button class="modal-btn modal-btn--danger" @click="confirmDelete">
              {{ t('mixer.outputMapDeleteConfirmDelete') }}
            </button>
          </div>
        </div>

        <p v-if="saveError" class="outmap-error">{{ saveError }}</p>
      </div>

      <footer class="modal-footer">
        <button class="modal-btn" :disabled="saving" @click="requestClose">
          {{ t('mixer.outputMapCancel') }}
        </button>
        <button class="modal-btn modal-btn--primary" :disabled="saving" @click="save">
          {{ saving ? t('mixer.outputMapSaving') : t('mixer.outputMapSave') }}
        </button>
      </footer>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed, ref, watch } from 'vue';

// The whole-map editor and the D3 "Remap Hardware Outputs" surface in one.
//
// Invariant 1 (server owns all control and state): the rows here are a
// *draft* of the server's outputs.json, never authoritative on their own.
// Saving is a single whole-map PUT (the endpoint replaces the map, so there
// is no partial-save path); on success the server broadcasts `outputs_changed`
// and every connected client — including this one, via MixerPanel — refetches
// and re-renders from that broadcast, never from anything held locally here.
// While the modal is open a second client's save is picked up the same way,
// which is why this component also subscribes to doc_patch itself rather
// than only reacting to its own submit.

interface DraftChannel { device: string; hwChannel: number }
interface DraftRow { key: number; name: string; channels: DraftChannel[]; builtin?: boolean }

const props = defineProps<{ open: boolean }>();
const emit = defineEmits<{ (e: 'close'): void }>();

const { t } = useLocalization();
const server = useLiveplayServer();

const rows = ref<DraftRow[]>([]);
let rowKeySeq = 0;
function freshKey() { return rowKeySeq++; }

const loadError = ref('');
const saveError = ref('');
const saving = ref(false);

async function loadAll() {
  loadError.value = '';
  try {
    await server.fetchDevices();
    const map = await server.fetchOutputs();
    const builtin = new Set(map?.builtin ?? []);
    rows.value = (map?.outputs ?? []).map(o => ({
      key: freshKey(),
      name: o.name,
      channels: (o.channels ?? []).map(c => ({ device: c.device, hwChannel: c.hwChannel })),
      builtin: builtin.has(o.name),
    }));
    // The built-in names (D26) are always shown, mapped or not, so "Main
    // Out" and "Preview Out" can be bound like any other name. An unmapped
    // one is a row with no channels; it is left out of the saved map (see
    // save) so the server keeps special-casing it rather than reading an
    // empty mapping.
    for (const name of [...(map?.builtin ?? [])].reverse()) {
      if (!rows.value.some(r => r.name === name)) {
        rows.value.unshift({ key: freshKey(), name, channels: [], builtin: true });
      }
    }
  } catch (e) {
    loadError.value = String(e);
  }
}

// Re-fetch on open, and keep listening for as long as the modal stays open —
// a second connected client (another mixer window, Companion, curl) may PUT
// its own map while this one is up, and D17 requires this view to converge
// too, not just the rail behind it.
let unsubDocPatch: (() => void) | null = null;
watch(() => props.open, (isOpen) => {
  if (isOpen) {
    saveError.value = '';
    pendingDelete.value = null;
    void loadAll();
    unsubDocPatch = server.onDocPatch((payload: any) => {
      if (payload?.op === 'outputs_changed') void loadAll();
    });
  } else {
    unsubDocPatch?.();
    unsubDocPatch = null;
  }
}, { immediate: true });

// The device this channel is currently set to may no longer be in the fresh
// device list (unplugged, or simply not opened yet) — offered anyway, same
// identity-fallback pattern MixerStrip uses for output names, so the select
// never renders blank for a channel that is still meaningfully configured.
function deviceOptions(current: string) {
  const list = server.devices ?? [];
  if (!current || list.some(d => d.id === current)) return list;
  return [...list, { id: current, display_name: current, channel_count: 0, sample_rate: 0, is_default: false }];
}

function addOutput() {
  let n = 1;
  const existing = new Set(rows.value.map(r => r.name));
  let name = t('mixer.outputMapNewOutputName', { n });
  while (existing.has(name)) { n += 1; name = t('mixer.outputMapNewOutputName', { n }); }
  rows.value.push({
    key: freshKey(),
    name,
    channels: [{ device: '', hwChannel: 0 }, { device: '', hwChannel: 1 }],
  });
}

function addChannel(row: DraftRow) {
  row.channels.push({ device: '', hwChannel: row.channels.length });
}

// Every bus currently pointing at an output name with no row at all, and
// reported unbound by the server (D10 — bound is server-computed, we only
// render it). Listed per bus, since two buses can share a missing target and
// an operator scanning strips wants to see both names.
const unmappedBusTargets = computed(() => {
  const mapped = new Set(rows.value.map(r => r.name));
  const out: Array<{ id: string; name: string; target: string }> = [];
  for (const b of server.buses ?? []) {
    if (!b.output || b.output.type !== 'output') continue;
    if (b.bound !== false) continue;
    const target = b.output.target;
    if (!target || mapped.has(target)) continue;
    out.push({ id: b.id, name: b.name, target });
  }
  return out;
});

function mapNow(target: string) {
  if (rows.value.some(r => r.name === target)) return;
  rows.value.push({
    key: freshKey(),
    name: target,
    channels: [{ device: '', hwChannel: 0 }, { device: '', hwChannel: 1 }],
  });
}

// Deleting a logical output that buses still reference must show the
// affected bus names before confirming — no orphan surprises. Cross-checked
// against the server's own bus list (not the draft), since that is where a
// stale reference would actually bite.
const pendingDelete = ref<{ index: number; name: string; affected: string[] } | null>(null);

function requestDeleteOutput(index: number) {
  const row = rows.value[index];
  if (!row) return;
  const affected = (server.buses ?? [])
    .filter(b => b.output?.type === 'output' && b.output.target === row.name)
    .map(b => b.name);
  if (affected.length) {
    pendingDelete.value = { index, name: row.name, affected };
  } else {
    rows.value.splice(index, 1);
  }
}
function confirmDelete() {
  if (pendingDelete.value) rows.value.splice(pendingDelete.value.index, 1);
  pendingDelete.value = null;
}
function cancelDelete() {
  pendingDelete.value = null;
}

async function save() {
  saveError.value = '';
  saving.value = true;
  try {
    const payload = {
      version: 1,
      outputs: rows.value
        .map(r => ({
          name: r.name.trim(),
          builtin: !!r.builtin,
          channels: r.channels
            .filter(c => c.device)
            .map(c => ({ device: c.device, hwChannel: Math.max(0, Math.trunc(c.hwChannel) || 0) })),
        }))
        // A built-in row with no channels is "unmapped", not "mapped to
        // nothing" — leaving it out is what keeps Main Out on the default
        // device.
        .filter(r => r.name && !(r.builtin && r.channels.length === 0))
        .map(({ name, channels }) => ({ name, channels })),
    };
    // Single whole-map PUT — the endpoint is a full replace, so there is no
    // partial-save path. On success the server's outputs_changed broadcast is
    // what every window (this one included, via MixerPanel) actually renders
    // from; we don't apply anything locally beyond closing the modal.
    await server.saveOutputs(payload);
    emit('close');
  } catch (e) {
    saveError.value = String(e);
  } finally {
    saving.value = false;
  }
}

function requestClose() {
  if (saving.value) return;
  emit('close');
}
</script>

<style scoped>
.outmap-backdrop {
  position: fixed;
  inset: 0;
  background: rgba(0, 0, 0, 0.55);
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: 1000;
}

.outmap-modal {
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: 10px;
  width: min(680px, 92vw);
  max-height: 90vh;
  overflow: hidden;
  display: flex;
  flex-direction: column;
  box-shadow: 0 12px 40px rgba(0, 0, 0, 0.35);
  color: var(--color-text-primary);
}

.modal-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 16px 20px;
  border-bottom: 1px solid var(--color-border);
}
.modal-header h2 { margin: 0; font-size: 18px; }
.close-x {
  background: none;
  border: none;
  color: var(--color-text-secondary);
  font-size: 18px;
  cursor: pointer;
}
.close-x:hover { color: var(--color-text-primary); }

.modal-body {
  padding: 16px 20px;
  display: flex;
  flex-direction: column;
  gap: 18px;
  overflow-y: auto;
}

.outmap-intro { margin: 0; font-size: 12px; color: var(--color-text-secondary); }
.outmap-error {
  margin: 0;
  font-size: 12px;
  color: var(--color-danger);
  padding: 8px 10px;
  background: var(--color-surface);
  border: 1px solid var(--color-danger);
  border-radius: 6px;
}

.outmap-section { display: flex; flex-direction: column; gap: 10px; }
.outmap-h3 {
  margin: 0;
  font-size: 12px;
  letter-spacing: 0.04em;
  text-transform: uppercase;
  color: var(--color-text-secondary);
}
.outmap-empty, .outmap-none {
  margin: 0;
  font-size: 12px;
  color: var(--color-text-disabled);
}

.outmap-row {
  display: flex;
  flex-direction: column;
  gap: 8px;
  padding: 10px;
  background: var(--color-surface);
  border: 1px solid var(--color-border);
  border-radius: 8px;
}
.outmap-rowhead { display: flex; align-items: center; gap: 6px; }
.outmap-name {
  flex: 1;
  min-width: 0;
  padding: 8px 10px;
  font-size: 13px;
  color: var(--color-text-primary);
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: 6px;
}
.outmap-name:focus { outline: none; border-color: var(--color-accent); }

.outmap-channels { display: flex; flex-direction: column; gap: 6px; }
.outmap-channel { display: flex; align-items: center; gap: 6px; }
.outmap-select {
  flex: 1;
  min-width: 0;
  padding: 6px 8px;
  font-size: 12px;
  color: var(--color-text-primary);
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: 6px;
}
.outmap-chnum {
  width: 56px;
  padding: 6px 8px;
  font-size: 12px;
  color: var(--color-text-primary);
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: 6px;
}

.outmap-iconbtn {
  display: flex;
  align-items: center;
  justify-content: center;
  flex: 0 0 auto;
  padding: 5px;
  color: var(--color-text-secondary);
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: 6px;
  cursor: pointer;
}
.outmap-builtin {
  display: flex;
  align-items: center;
  color: var(--color-text-disabled);
  cursor: help;
}
.outmap-builtin .material-symbols-rounded { font-size: 16px; }
.outmap-iconbtn:hover { color: var(--color-text-primary); }
.outmap-iconbtn .material-symbols-rounded { font-size: 16px; }
.outmap-iconbtn--danger:hover { color: var(--color-danger); border-color: var(--color-danger); }

.outmap-addchannel,
.outmap-addoutput {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 4px;
  align-self: flex-start;
  padding: 6px 10px;
  font-size: 12px;
  color: var(--color-accent);
  background: var(--color-background);
  border: 1px dashed var(--color-accent);
  border-radius: 6px;
  cursor: pointer;
}
.outmap-addchannel .material-symbols-rounded,
.outmap-addoutput .material-symbols-rounded { font-size: 15px; }
.outmap-addoutput { align-self: stretch; justify-content: center; }

.outmap-unmapped-list { list-style: none; margin: 0; padding: 0; display: flex; flex-direction: column; gap: 6px; }
.outmap-unmapped-item {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 8px 10px;
  font-size: 12px;
  background: var(--color-surface);
  border: 1px solid var(--color-warning);
  border-radius: 6px;
}
.outmap-unmapped-bus { color: var(--color-text-primary); font-weight: 500; }
.outmap-unmapped-target { color: var(--color-text-secondary); flex: 1; min-width: 0; }
.outmap-mapnow {
  flex: 0 0 auto;
  padding: 4px 10px;
  font-size: 11px;
  color: #fff;
  background: var(--color-accent);
  border: none;
  border-radius: 6px;
  cursor: pointer;
}

.outmap-confirm {
  display: flex;
  flex-direction: column;
  gap: 8px;
  padding: 12px;
  background: var(--color-surface);
  border: 1px solid var(--color-danger);
  border-radius: 8px;
}
.outmap-confirm-title { margin: 0; font-size: 13px; font-weight: 600; color: var(--color-text-primary); }
.outmap-confirm-body { margin: 0; font-size: 12px; color: var(--color-text-secondary); }
.outmap-confirm-buses {
  margin: 0;
  padding-left: 18px;
  font-size: 12px;
  color: var(--color-text-primary);
  max-height: 120px;
  overflow-y: auto;
}
.outmap-confirm-actions { display: flex; justify-content: flex-end; gap: 8px; }

.modal-footer {
  padding: 12px 20px;
  border-top: 1px solid var(--color-border);
  display: flex;
  justify-content: flex-end;
  gap: 8px;
}
.modal-btn {
  background: var(--color-surface);
  border: 1px solid var(--color-border);
  border-radius: 6px;
  color: var(--color-text-primary);
  padding: 8px 16px;
  cursor: pointer;
  font-size: 14px;
}
.modal-btn:hover { background: var(--color-surface-hover); }
.modal-btn:disabled { opacity: 0.5; cursor: not-allowed; }
.modal-btn--primary { background: var(--color-accent); border-color: var(--color-accent); color: #fff; }
.modal-btn--primary:hover { background: var(--color-accent); opacity: 0.9; }
.modal-btn--danger { background: var(--color-danger); border-color: var(--color-danger); color: #fff; }
</style>

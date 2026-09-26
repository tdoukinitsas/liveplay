<template>
  <div class="settings-pane outmap-pane">
      <h3 class="settings-pane-title">{{ t('settings.sectionOutputs') }}</h3>
      <p class="settings-pane-intro">{{ t('mixer.remap.intro') }}</p>

      <div class="outmap-body">
        <p v-if="loadError" class="outmap-error">{{ loadError }}</p>

        <div class="remap-cols">
          <!-- ============ Left: the outputs this show needs ============ -->
          <section class="remap-col">
            <h3 class="outmap-h3">{{ t('mixer.remap.missingHeading') }}</h3>

            <div v-if="loaded && missingSlots.length === 0" class="remap-allgood">
              <span class="material-symbols-rounded">check_circle</span>
              <span>{{ t('mixer.remap.allConnected') }}</span>
            </div>

            <div v-else class="remap-slots">
              <div
                v-for="slot in missingSlots"
                :key="'m:' + slot.name"
                v-bind="slotAttrs(slot)"
                v-on="slotHandlers(slot)"
              >
                <div class="slot__head">
                  <span class="material-symbols-rounded slot__icon">
                    {{ pendingFor(slot.name) ? 'link' : 'link_off' }}
                  </span>
                  <span class="slot__name" :title="slot.name">{{ slot.name }}</span>
                  <span v-if="!pendingFor(slot.name)" class="slot__badge">{{ t('mixer.remap.missingBadge') }}</span>
                </div>
                <div class="slot__why">
                  {{ kindText(slot) }}<template v-if="slot.kind === 'absent'"> · {{ t('mixer.remap.was', { mapping: describeChannels(slot.baseline) }) }}</template>
                </div>
                <div v-if="slot.buses.length" class="slot__buses">
                  <span v-for="b in slot.buses" :key="b.id" class="chip">
                    <span class="chip__dot" :style="{ background: b.color || 'var(--color-text-disabled)' }" />
                    {{ b.name }}
                  </span>
                </div>
                <div v-if="pendingFor(slot.name)" class="slot__pending">
                  <span class="slot__arrow">→ {{ describePending(slot.name) }}</span>
                  <button
                    class="slot__undo"
                    :title="t('mixer.remap.undo')"
                    :aria-label="t('mixer.remap.undo')"
                    @click.stop="undo(slot.name)"
                    @keydown.stop
                  >
                    <span class="material-symbols-rounded">close</span>
                  </button>
                </div>
                <div v-else-if="dropTarget === slot.name || selectedSlot === slot.name" class="slot__hint">
                  {{ dropTarget === slot.name ? t('mixer.remap.dropHere') : t('mixer.remap.pickAvailable') }}
                </div>
              </div>
            </div>

            <template v-if="otherSlots.length">
              <h3 class="outmap-h3 remap-h3--second">{{ t('mixer.remap.otherHeading') }}</h3>
              <div class="remap-slots">
                <div
                  v-for="slot in otherSlots"
                  :key="'o:' + slot.name"
                  v-bind="slotAttrs(slot)"
                  v-on="slotHandlers(slot)"
                >
                  <div class="slot__head">
                    <span class="material-symbols-rounded slot__icon">speaker</span>
                    <span class="slot__name" :title="slot.name">{{ slot.name }}</span>
                  </div>
                  <div v-if="!pendingFor(slot.name)" class="slot__why">{{ currentText(slot) }}</div>
                  <div v-if="slot.buses.length" class="slot__buses">
                    <span v-for="b in slot.buses" :key="b.id" class="chip">
                      <span class="chip__dot" :style="{ background: b.color || 'var(--color-text-disabled)' }" />
                      {{ b.name }}
                    </span>
                  </div>
                  <div v-if="pendingFor(slot.name)" class="slot__pending">
                    <span class="slot__arrow">→ {{ describePending(slot.name) }}</span>
                    <button
                      class="slot__undo"
                      :title="t('mixer.remap.undo')"
                      :aria-label="t('mixer.remap.undo')"
                      @click.stop="undo(slot.name)"
                      @keydown.stop
                    >
                      <span class="material-symbols-rounded">close</span>
                    </button>
                  </div>
                  <div v-else-if="dropTarget === slot.name || selectedSlot === slot.name" class="slot__hint">
                    {{ dropTarget === slot.name ? t('mixer.remap.dropHere') : t('mixer.remap.pickAvailable') }}
                  </div>
                </div>
              </div>
            </template>
          </section>

          <!-- ============ Right: what this machine actually has ============ -->
          <section class="remap-col">
            <h3 class="outmap-h3">{{ t('mixer.remap.availableHeading') }}</h3>
            <p v-if="selectedCard" class="remap-pickhint">{{ t('mixer.remap.pickSlot') }}</p>

            <p v-if="!loaded" class="outmap-none">{{ t('mixer.remap.loading') }}</p>
            <p v-else-if="hwDevices.length === 0" class="outmap-none">{{ t('mixer.remap.noDevices') }}</p>

            <div v-else class="hw-list">
              <div v-for="dev in hwDevices" :key="dev.id" class="hwdev" :class="{ 'hwdev--multi': dev.cards.length > 1 || dev.channelCount > 2 }">
                <div v-if="dev.channelCount > 2" class="hwdev__head">
                  <span class="hwdev__name">{{ dev.name }}</span>
                  <span v-if="dev.isDefault" class="hw__badge">{{ t('mixer.remap.defaultBadge') }}</span>
                  <span class="hwdev__count">{{ t('mixer.remap.channelCount', { n: dev.channelCount }) }}</span>
                </div>
                <div class="hwdev__cards">
                  <button
                    v-for="card in dev.cards"
                    :key="card.key"
                    type="button"
                    class="hw"
                    :class="{
                      'hw--selected': selectedCard === card.key,
                      'hw--dragging': draggingKey === card.key,
                      'hw--used': usedBy(card).length > 0,
                    }"
                    draggable="true"
                    :aria-pressed="selectedCard === card.key"
                    :title="selectedSlot ? t('mixer.remap.assignTo', { name: selectedSlot }) : ''"
                    @click="onCardActivate(card)"
                    @dragstart="onDragStart($event, card)"
                    @dragend="onDragEnd"
                  >
                    <span class="material-symbols-rounded hw__grip">drag_indicator</span>
                    <span class="hw__text">
                      <span class="hw__label">
                        <span class="hw__name">{{ card.label }}</span>
                        <span v-if="dev.channelCount <= 2 && dev.isDefault" class="hw__badge">{{ t('mixer.remap.defaultBadge') }}</span>
                      </span>
                      <span v-if="usedBy(card).length" class="hw__used">
                        {{ t('mixer.remap.inUse', { names: usedBy(card).join(', ') }) }}
                      </span>
                    </span>
                  </button>
                </div>
              </div>
            </div>
          </section>
        </div>

        <!-- ================= Advanced: the per-channel editor ================= -->
        <section class="remap-advanced">
          <button
            type="button"
            class="remap-advanced__toggle"
            :aria-expanded="advancedOpen"
            @click="advancedOpen = !advancedOpen"
          >
            <span class="material-symbols-rounded remap-advanced__chev" :class="{ 'is-open': advancedOpen }">chevron_right</span>
            <span class="remap-advanced__title">{{ t('mixer.remap.advanced') }}</span>
            <span class="remap-advanced__hint">{{ t('mixer.remap.advancedHint') }}</span>
          </button>

          <div v-if="advancedOpen" class="outmap-section">
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

            <!-- Delete confirmation -->
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
          </div>
        </section>

        <p v-if="saveError" class="outmap-error">{{ saveError }}</p>

        <!-- The hardware this machine actually has. P3b deliberately left this
             out of the Server pane and reserved its three keys for here: a
             device list belongs beside the map that points at it, not with the
             machine's ports and origins. Read-only except for Open, which asks
             the server to hold the device open — useful when an interface only
             appears to the system once something has claimed it. -->
        <section class="outmap-devices">
          <h3 class="outmap-h3">{{ t('serverSettings.outputDevices') }}</h3>
          <p v-if="!server.devices.value.length" class="outmap-none">
            {{ t('serverSettings.noDevices') }}
          </p>
          <ul v-else class="outmap-devlist">
            <li v-for="d in server.devices.value" :key="d.id" class="outmap-dev">
              <span class="outmap-dev__name" :title="d.display_name">{{ d.display_name }}</span>
              <span v-if="d.is_default" class="hw__badge">{{ t('mixer.remap.defaultBadge') }}</span>
              <span class="outmap-dev__count">
                {{ t('mixer.remap.channelCount', { n: d.channel_count }) }}
              </span>
              <button
                type="button"
                class="outmap-iconbtn"
                :title="t('serverSettings.open')"
                @click="onOpenDevice(d)"
              >
                <span class="material-symbols-rounded">power_settings_new</span>
              </button>
            </li>
          </ul>
        </section>
      </div>

      <!-- A pane has no "cancel and close", so Cancel becomes Revert: it
           re-reads the server's map and throws this draft away. Both are
           disabled until something is actually different, because a Save that
           would send the map back unchanged invites the question of whether it
           did anything. -->
      <div class="settings-actions outmap-actions">
        <button class="modal-btn" :disabled="saving || !isDirty" @click="revert">
          {{ t('mixer.outputMapRevert') }}
        </button>
        <button class="modal-btn modal-btn--primary" :disabled="saving || !isDirty" @click="save">
          {{ saving ? t('mixer.outputMapSaving') : t('mixer.outputMapSave') }}
        </button>
      </div>
  </div>
</template>

<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from 'vue';
import type { Bus } from '~/types/project';
import type { OutputMapChannel } from '~/composables/useLiveplayServer';

// "Remap Hardware Outputs": missing outputs on the left, this machine's
// physical outputs on the right, drag (or click, click) one onto the other.
//
// Invariant 1 (server owns all control and state): everything here is a
// *draft* of the server's outputs.json, never authoritative on its own.
// There is exactly one draft — `rows`, the same list the Advanced per-channel
// editor edits — and the simple view is a second way of writing into it: a
// drop sets one row's channels to the dropped pair. What the slots show as
// "pending" is simply every row that differs from the map as loaded
// (`baseline`), so both editors always agree about what Save will send.
//
// Saving is a single whole-map PUT (the endpoint replaces the map, so there
// is no partial-save path); on success the server broadcasts `outputs_changed`
// and every connected client — including this one, via MixerPanel — refetches
// and re-renders from that broadcast, never from anything held locally here.
//
// What counts as missing is the server's call, never ours: a bus is listed
// when it reports `bound === false` (D10). The server matches renumbered
// Windows device names itself, so a bus whose device merely changed its
// "N- " prefix is bound and correctly absent from the list.

interface DraftChannel { device: string; hwChannel: number }
interface DraftRow { key: number; name: string; channels: DraftChannel[]; builtin?: boolean }

/** One output name a pair can be dropped onto. */
interface Slot {
  name: string;
  /**
   * absent  — outputs.json maps it, but to hardware that isn't here
   * unknown — nothing on this machine defines it
   * silent  — the built-in Preview Out, unmapped (silent by design)
   * ok      — not missing; listed so it can be re-mapped proactively
   */
  kind: 'absent' | 'unknown' | 'silent' | 'ok';
  buses: Bus[];
  /** Its channels in the map as loaded (empty when it has no entry). */
  baseline: DraftChannel[];
}

/** One physical destination: a whole 1–2 channel device, or one pair of a wider one. */
interface HwCard {
  key: string;
  deviceId: string;
  deviceName: string;
  label: string;
  channels: number[];
}

const BUILTIN_MAIN = 'Main Out';
const BUILTIN_PREVIEW = 'Preview Out';
const DRAG_MIME = 'application/x-liveplay-hwpair';

const { t } = useLocalization();
const server = useLiveplayServer();

const rows = ref<DraftRow[]>([]);
/** The map as last loaded from the server, by name. */
const baseline = ref<Map<string, DraftChannel[]>>(new Map());
const builtinNames = ref<string[]>([]);
let rowKeySeq = 0;
function freshKey() { return rowKeySeq++; }

const loaded = ref(false);
const loadError = ref('');
const saveError = ref('');
const saving = ref(false);
const advancedOpen = ref(false);

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

/**
 * Fetch devices, buses and the map. `keepPending` re-applies this window's
 * un-saved changes on top of the fresh map — used when another client saves
 * while the modal is open, so their change lands without eating ours.
 */
async function loadAll(keepPending = false) {
  loadError.value = '';
  const carried = keepPending ? pendingChanges() : [];
  try {
    // Devices first: `bound` is computed against the device list the server
    // refreshes on GET /api/devices, so buses are fetched after it.
    await server.fetchDevices();
    const [map] = await Promise.all([server.fetchOutputs(), server.fetchBuses()]);
    const builtin = map?.builtin ?? [];
    builtinNames.value = [...builtin];
    const base = new Map<string, DraftChannel[]>();
    for (const o of map?.outputs ?? []) base.set(o.name, cloneChannels(o.channels ?? []));
    baseline.value = base;

    const bset = new Set(builtin);
    rows.value = (map?.outputs ?? []).map(o => ({
      key: freshKey(),
      name: o.name,
      channels: cloneChannels(o.channels ?? []),
      builtin: bset.has(o.name),
    }));
    // The built-in names (D26) are always shown, mapped or not, so "Main
    // Out" and "Preview Out" can be bound like any other name. An unmapped
    // one is a row with no channels; it is left out of the saved map (see
    // save) so the server keeps special-casing it rather than reading an
    // empty mapping.
    for (const name of [...builtin].reverse()) {
      if (!rows.value.some(r => r.name === name)) {
        rows.value.unshift({ key: freshKey(), name, channels: [], builtin: true });
      }
    }
    for (const c of carried) setRowChannels(c.name, c.channels);
    loaded.value = true;
    // Re-baseline the dirty check against what was just loaded. Done here
    // rather than in save() so it is also correct after a revert and after
    // another client's outputs_changed lands.
    loadedSignature.value = draftSignature.value;
  } catch (e) {
    loadError.value = String(e);
  }
}

// Has this draft diverged from the map as loaded? Compared on the SAVED shape
// — trimmed names and cleaned channels — rather than the raw rows, so typing a
// space into a name or adding an empty channel row does not arm Save for a
// change that would not survive being sent.
const draftSignature = computed(() => JSON.stringify(
  rows.value.map(r => ({ n: r.name.trim(), c: cleanChannels(r.channels) })),
));
const loadedSignature = ref('');
const isDirty = computed(() => loaded.value && draftSignature.value !== loadedSignature.value);


// ---------------------------------------------------------------------------
// Draft helpers
// ---------------------------------------------------------------------------

function cloneChannels(chs: OutputMapChannel[] | DraftChannel[]): DraftChannel[] {
  return chs.map(c => ({ device: c.device, hwChannel: c.hwChannel }));
}

/** Channels as Save would send them. */
function cleanChannels(chs: DraftChannel[]): DraftChannel[] {
  return chs
    .filter(c => c.device)
    .map(c => ({ device: c.device, hwChannel: Math.max(0, Math.trunc(c.hwChannel) || 0) }));
}

function sameChannels(a: DraftChannel[], b: DraftChannel[]) {
  return a.length === b.length && a.every((c, i) => c.device === b[i]!.device && c.hwChannel === b[i]!.hwChannel);
}

function rowFor(name: string) {
  return rows.value.find(r => r.name.trim() === name);
}

/** The draft's channels for `name` when they differ from the loaded map, else null. */
function pendingFor(name: string): DraftChannel[] | null {
  const row = rowFor(name);
  const draft = row ? cleanChannels(row.channels) : [];
  const base = baseline.value.get(name) ?? [];
  return sameChannels(draft, base) ? null : draft;
}

function pendingChanges() {
  const out: Array<{ name: string; channels: DraftChannel[] }> = [];
  for (const r of rows.value) {
    const name = r.name.trim();
    if (!name) continue;
    const p = pendingFor(name);
    if (p) out.push({ name, channels: p });
  }
  return out;
}

function setRowChannels(name: string, channels: DraftChannel[]) {
  const row = rowFor(name);
  if (row) row.channels = cloneChannels(channels);
  else rows.value.push({
    key: freshKey(),
    name,
    channels: cloneChannels(channels),
    builtin: builtinNames.value.includes(name),
  });
}

/** Put `name` back the way the loaded map has it. */
function undo(name: string) {
  const base = baseline.value.get(name);
  const row = rowFor(name);
  if (!row) return;
  if (base || row.builtin) row.channels = cloneChannels(base ?? []);
  else rows.value.splice(rows.value.indexOf(row), 1);
}

function assign(slotName: string, card: HwCard) {
  setRowChannels(slotName, card.channels.map(hw => ({ device: card.deviceId, hwChannel: hw })));
  selectedSlot.value = null;
  selectedCard.value = null;
}

// ---------------------------------------------------------------------------
// Labels
// ---------------------------------------------------------------------------

function deviceLabel(id: string): string {
  return (server.devices ?? []).find(d => d.id === id)?.display_name ?? id;
}

// "UMC1820 ch 3/4" — channels 1-based, as they're printed on the interface.
function describeChannels(chs: DraftChannel[]): string {
  if (!chs.length) return t('mixer.remap.unmapped');
  const ch = t('mixer.remap.channelsShort');
  const devices = [...new Set(chs.map(c => c.device))];
  if (devices.length === 1) {
    return `${deviceLabel(devices[0]!)} ${ch} ${chs.map(c => c.hwChannel + 1).join('/')}`;
  }
  return chs.map(c => `${deviceLabel(c.device)} ${ch} ${c.hwChannel + 1}`).join(' + ');
}

function describePending(name: string) {
  return describeChannels(pendingFor(name) ?? []);
}

function kindText(slot: Slot) {
  if (slot.kind === 'absent') return t('mixer.remap.kindAbsent');
  if (slot.kind === 'silent') return t('mixer.remap.kindSilent');
  return t('mixer.remap.kindUnknown');
}

function currentText(slot: Slot) {
  if (slot.baseline.length) return describeChannels(slot.baseline);
  if (slot.name === BUILTIN_MAIN) return t('mixer.remap.currentDefault');
  if (slot.name === BUILTIN_PREVIEW) return t('mixer.remap.currentSilent');
  return t('mixer.remap.currentNone');
}

// ---------------------------------------------------------------------------
// Slots (left)
// ---------------------------------------------------------------------------

const busesByOrder = computed<Bus[]>(() =>
  [...(server.buses ?? [])].sort((a, b) => a.order - b.order));

const missingSlots = computed<Slot[]>(() => {
  const byName = new Map<string, Slot>();
  for (const b of busesByOrder.value) {
    // Bus→bus sends are unbound only because their terminal bus is; that
    // bus is listed in its own right.
    if (b.output?.type !== 'output' || b.bound !== false) continue;
    const name = b.output.target;
    if (!name) continue;
    let slot = byName.get(name);
    if (!slot) {
      const base = baseline.value.get(name) ?? [];
      const kind: Slot['kind'] = base.length ? 'absent'
        : name === BUILTIN_PREVIEW ? 'silent'
        : 'unknown';
      slot = { name, kind, buses: [], baseline: base };
      byName.set(name, slot);
    }
    slot.buses.push(b);
  }
  return [...byName.values()];
});

// Every other name that means something here — the built-ins and the map's
// own entries — so a working output can be re-pointed too. Plus any name
// with a staged change, so nothing Save will send is ever off-screen (a slot
// stops being "missing" when another client re-routes its bus mid-edit).
const otherSlots = computed<Slot[]>(() => {
  const missing = new Set(missingSlots.value.map(s => s.name));
  const names: string[] = [];
  for (const n of builtinNames.value) if (!names.includes(n)) names.push(n);
  for (const n of baseline.value.keys()) if (!names.includes(n)) names.push(n);
  for (const r of rows.value) {
    const n = r.name.trim();
    if (n && !names.includes(n) && pendingFor(n)) names.push(n);
  }
  return names
    .filter(n => !missing.has(n))
    .map(name => ({
      name,
      kind: 'ok' as const,
      buses: busesByOrder.value.filter(b => b.output?.type === 'output' && b.output.target === name),
      baseline: baseline.value.get(name) ?? [],
    }));
});

// ---------------------------------------------------------------------------
// Physical outputs (right)
// ---------------------------------------------------------------------------

const hwDevices = computed(() => {
  const seen = new Set<string>();
  const out: Array<{ id: string; name: string; isDefault: boolean; channelCount: number; cards: HwCard[] }> = [];
  for (const d of server.devices ?? []) {
    // The OS can enumerate one endpoint twice; one entry is enough.
    if (!d.id || seen.has(d.id)) continue;
    seen.add(d.id);
    const name = d.display_name || d.id;
    const n = Math.max(1, d.channel_count || 0);
    const cards: HwCard[] = [];
    if (n <= 2) {
      const chs = n === 1 ? [0] : [0, 1];
      cards.push({ key: `${d.id}#${chs.join(',')}`, deviceId: d.id, deviceName: name, label: name, channels: chs });
    } else {
      for (let k = 0; k < n; k += 2) {
        const chs = k + 1 < n ? [k, k + 1] : [k];
        cards.push({
          key: `${d.id}#${chs.join(',')}`,
          deviceId: d.id,
          deviceName: name,
          label: chs.length === 2
            ? t('mixer.remap.pair', { a: k + 1, b: k + 2 })
            : t('mixer.remap.mono', { a: k + 1 }),
          channels: chs,
        });
      }
    }
    out.push({ id: d.id, name, isDefault: !!d.is_default, channelCount: n, cards });
  }
  return out;
});

const cardByKey = computed(() => {
  const m = new Map<string, HwCard>();
  for (const d of hwDevices.value) for (const c of d.cards) m.set(c.key, c);
  return m;
});

/** Output names (in the draft) that already send to this card's channels. */
function usedBy(card: HwCard): string[] {
  const names: string[] = [];
  for (const r of rows.value) {
    const chs = cleanChannels(r.channels);
    const name = r.name.trim();
    if (!name) continue;
    if (chs.some(c => c.device === card.deviceId && card.channels.includes(c.hwChannel))) names.push(name);
    // Main Out with no mapping plays on the default device's first pair.
    else if (!chs.length && name === BUILTIN_MAIN && card.channels.includes(0)
      && (server.devices ?? []).find(d => d.id === card.deviceId)?.is_default) names.push(name);
  }
  return names;
}

// ---------------------------------------------------------------------------
// Interaction: drag-and-drop, and click-then-click as its equal
// ---------------------------------------------------------------------------

const selectedSlot = ref<string | null>(null);
const selectedCard = ref<string | null>(null);
const draggingKey = ref<string | null>(null);
const dropTarget = ref<string | null>(null);

function onSlotActivate(slot: Slot) {
  const card = selectedCard.value ? cardByKey.value.get(selectedCard.value) : null;
  if (card) { assign(slot.name, card); return; }
  selectedSlot.value = selectedSlot.value === slot.name ? null : slot.name;
}

function onCardActivate(card: HwCard) {
  if (selectedSlot.value) { assign(selectedSlot.value, card); return; }
  selectedCard.value = selectedCard.value === card.key ? null : card.key;
}

function onDragStart(e: DragEvent, card: HwCard) {
  draggingKey.value = card.key;
  selectedCard.value = null;
  if (e.dataTransfer) {
    e.dataTransfer.effectAllowed = 'copy';
    e.dataTransfer.setData(DRAG_MIME, card.key);
    e.dataTransfer.setData('text/plain', `${card.deviceName} ${card.label}`);
  }
}

function onDragEnd() {
  draggingKey.value = null;
  dropTarget.value = null;
}

function isOurDrag(e: DragEvent) {
  return draggingKey.value !== null || !!e.dataTransfer?.types?.includes(DRAG_MIME);
}

function onSlotDragOver(e: DragEvent, slot: Slot) {
  if (!isOurDrag(e)) return;
  e.preventDefault();
  e.stopPropagation();
  if (e.dataTransfer) e.dataTransfer.dropEffect = 'copy';
  dropTarget.value = slot.name;
}

function onSlotDragLeave(e: DragEvent, slot: Slot) {
  const to = e.relatedTarget as Node | null;
  if (to && (e.currentTarget as HTMLElement).contains(to)) return;
  if (dropTarget.value === slot.name) dropTarget.value = null;
}

function onSlotDrop(e: DragEvent, slot: Slot) {
  if (!isOurDrag(e)) return;
  e.preventDefault();
  e.stopPropagation();
  const key = e.dataTransfer?.getData(DRAG_MIME) || draggingKey.value;
  const card = key ? cardByKey.value.get(key) : null;
  if (card) assign(slot.name, card);
  onDragEnd();
}

function slotAttrs(slot: Slot) {
  const missing = slot.kind !== 'ok';
  return {
    class: [
      'slot',
      missing ? 'slot--missing' : 'slot--other',
      {
        'slot--pending': !!pendingFor(slot.name),
        'slot--selected': selectedSlot.value === slot.name,
        'slot--drop': dropTarget.value === slot.name,
        'slot--armed': draggingKey.value !== null || selectedCard.value !== null,
      },
    ],
    role: 'button',
    tabindex: 0,
    'aria-pressed': selectedSlot.value === slot.name,
  };
}

function slotHandlers(slot: Slot) {
  return {
    click: () => onSlotActivate(slot),
    keydown: (e: KeyboardEvent) => {
      if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); onSlotActivate(slot); }
    },
    dragenter: (e: DragEvent) => onSlotDragOver(e, slot),
    dragover: (e: DragEvent) => onSlotDragOver(e, slot),
    dragleave: (e: DragEvent) => onSlotDragLeave(e, slot),
    drop: (e: DragEvent) => onSlotDrop(e, slot),
  };
}

// ---------------------------------------------------------------------------
// Advanced editor (unchanged behaviour)
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Save
// ---------------------------------------------------------------------------

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
          channels: cleanChannels(r.channels),
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
    // from; we don't apply anything locally.
    //
    // The pane stays open afterwards, unlike the modal it replaces, so it
    // re-reads rather than closing: that both re-baselines the dirty check and
    // shows the operator the map as the server now holds it, which is the
    // thing they were editing towards.
    await server.saveOutputs(payload);
    await loadAll();
  } catch (e) {
    saveError.value = String(e);
  } finally {
    saving.value = false;
  }
}

/** Throw this draft away and re-read the server's map. */
function revert() {
  if (saving.value) return;
  saveError.value = '';
  pendingDelete.value = null;
  selectedSlot.value = null;
  selectedCard.value = null;
  void loadAll();
}

// Ask the server to hold a device open. Reported through saveError rather than
// silently, for the same reason every refusal in this pane is: a button that
// does nothing visible is indistinguishable from one that failed.
async function onOpenDevice(d: { display_name: string; channel_count: number }) {
  saveError.value = '';
  try {
    await server.openDevice(d.display_name, Math.max(2, d.channel_count));
    await server.fetchDevices();
  } catch (e) {
    saveError.value = String(e);
  }
}

// As a pane this is mounted only while it is on screen, so the modal's
// `watch(() => props.open)` becomes a plain mount/unmount pair. The ordering
// hazard that comment recorded is gone with it: the close branch used to run
// immediately on a modal that starts closed and reach drag state declared
// further down the file, which threw "Cannot access before initialization" and
// took the detached mixer window down with a 500. onMounted cannot run early.
//
// The subscription stays for the pane's lifetime for the original reason: a
// second connected client (another mixer window, Companion, curl) may PUT its
// own map while this is open, and D17 requires this view to converge too, not
// just the rail behind it. `loadAll(true)` re-applies this window's unsaved
// edits on top of theirs.
let unsubDocPatch: (() => void) | null = null;
onMounted(() => {
  void loadAll();
  unsubDocPatch = server.onDocPatch((payload: any) => {
    if (payload?.op === 'outputs_changed' && !saving.value) void loadAll(true);
  });
});
onBeforeUnmount(() => {
  unsubDocPatch?.();
  unsubDocPatch = null;
  onDragEnd();
});
</script>

<style scoped>
/* This pane overrides the 620px measure SettingsPage gives every other pane.
   That figure is there because settings read like prose and a full-width field
   on a 2560px display looks unfinished — but this is not prose. It is two
   columns you drag between, and squeezing them into a reading measure would
   make the one interaction the pane exists for harder for no gain. */
.outmap-pane {
  max-width: 1100px;
}

.outmap-body {
  display: flex;
  flex-direction: column;
  gap: 16px;
}
.outmap-error {
  margin: 0;
  font-size: 12px;
  color: var(--color-danger);
  padding: 8px 10px;
  background: var(--color-surface);
  border: 1px solid var(--color-danger);
  border-radius: 6px;
}

.outmap-h3 {
  margin: 0;
  font-size: 12px;
  letter-spacing: 0.04em;
  text-transform: uppercase;
  color: var(--color-text-secondary);
}
.remap-h3--second { margin-top: var(--spacing-sm, 8px); }
.outmap-empty, .outmap-none {
  margin: 0;
  font-size: 12px;
  color: var(--color-text-disabled);
}

/* ---------------- Two columns ---------------- */
.remap-cols {
  display: grid;
  grid-template-columns: minmax(0, 1fr) minmax(0, 1fr);
  gap: 20px;
  align-items: start;
}
@media (max-width: 700px) {
  .remap-cols { grid-template-columns: minmax(0, 1fr); }
}
.remap-col { display: flex; flex-direction: column; gap: 10px; min-width: 0; }

.remap-allgood {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 12px;
  font-size: 13px;
  color: var(--color-text-primary);
  background: var(--color-surface);
  border: 1px solid var(--color-success);
  border-radius: 8px;
}
.remap-allgood .material-symbols-rounded { color: var(--color-success); font-size: 20px; }

.remap-pickhint {
  margin: 0;
  font-size: 12px;
  color: var(--color-accent);
}

/* ---------------- Slots (left) ---------------- */
.remap-slots { display: flex; flex-direction: column; gap: 8px; }

.slot {
  display: flex;
  flex-direction: column;
  gap: 6px;
  padding: 10px 12px;
  background: var(--color-surface);
  border: 1px solid var(--color-border);
  border-radius: 8px;
  cursor: pointer;
  outline: none;
  transition: border-color var(--transition-fast, 0.12s), background var(--transition-fast, 0.12s), box-shadow var(--transition-fast, 0.12s);
}
.slot:hover { background: var(--color-surface-hover); }
.slot:focus-visible { box-shadow: 0 0 0 2px var(--color-accent); }

.slot--missing {
  border-color: var(--color-warning);
  border-left-width: 4px;
}
/* Staged: will be fixed on Save. */
.slot--pending { border-color: var(--color-success); }
.slot--other { padding: 8px 12px; }

/* Something is in hand (a drag, or a card picked first): every slot shows
   it can take it. */
.slot--armed { border-style: dashed; }
.slot--missing.slot--armed { border-left-style: solid; }
.slot--selected,
.slot--drop {
  border-color: var(--color-accent);
  border-style: solid;
  background: color-mix(in srgb, var(--color-accent) 12%, var(--color-surface));
  box-shadow: 0 0 0 1px var(--color-accent);
}

.slot__head { display: flex; align-items: center; gap: 8px; min-width: 0; }
.slot__icon { font-size: 18px; color: var(--color-text-secondary); flex: 0 0 auto; }
.slot--missing .slot__icon { color: var(--color-warning); }
.slot--pending .slot__icon { color: var(--color-success); }
.slot__name {
  font-size: 14px;
  font-weight: 600;
  min-width: 0;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.slot__badge {
  margin-left: auto;
  flex: 0 0 auto;
  padding: 1px 8px;
  font-size: 11px;
  font-weight: 600;
  color: var(--color-warning);
  border: 1px solid var(--color-warning);
  border-radius: 999px;
}
.slot__why { font-size: 12px; color: var(--color-text-secondary); overflow-wrap: anywhere; }

.slot__buses { display: flex; flex-wrap: wrap; gap: 4px; }
.chip {
  display: inline-flex;
  align-items: center;
  gap: 5px;
  padding: 2px 8px;
  font-size: 11px;
  color: var(--color-text-primary);
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: 999px;
  max-width: 100%;
}
.chip__dot { width: 8px; height: 8px; border-radius: 50%; flex: 0 0 auto; }

.slot__pending {
  display: flex;
  align-items: center;
  gap: 6px;
  padding: 5px 6px 5px 10px;
  font-size: 12px;
  font-weight: 500;
  color: var(--color-success);
  background: var(--color-background);
  border: 1px solid var(--color-success);
  border-radius: 6px;
}
.slot__arrow { flex: 1; min-width: 0; overflow-wrap: anywhere; line-height: 1.3; }
.slot__undo {
  display: flex;
  align-items: center;
  justify-content: center;
  flex: 0 0 auto;
  padding: 2px;
  color: var(--color-text-secondary);
  background: none;
  border: none;
  border-radius: 4px;
  cursor: pointer;
}
.slot__undo:hover, .slot__undo:focus-visible { color: var(--color-text-primary); background: var(--color-surface-hover); outline: none; }
.slot__undo .material-symbols-rounded { font-size: 16px; }
.slot__hint {
  padding: 5px 10px;
  font-size: 12px;
  color: var(--color-accent);
  border: 1px dashed var(--color-accent);
  border-radius: 6px;
  text-align: center;
}

/* ---------------- Physical outputs (right) ---------------- */
.hw-list { display: flex; flex-direction: column; gap: 8px; }
.hwdev { display: flex; flex-direction: column; gap: 6px; }
.hwdev--multi {
  padding: 8px;
  background: var(--color-surface);
  border: 1px solid var(--color-border);
  border-radius: 8px;
}
.hwdev__head { display: flex; align-items: center; gap: 6px; min-width: 0; font-size: 12px; }
.hwdev__name {
  font-weight: 600;
  min-width: 0;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.hwdev__count { margin-left: auto; flex: 0 0 auto; color: var(--color-text-secondary); font-size: 11px; }
.hwdev__cards { display: grid; grid-template-columns: minmax(0, 1fr); gap: 6px; }
.hwdev--multi .hwdev__cards { grid-template-columns: repeat(auto-fill, minmax(120px, 1fr)); }

.hw {
  display: flex;
  align-items: center;
  gap: 6px;
  width: 100%;
  min-width: 0;
  padding: 8px 10px 8px 4px;
  text-align: left;
  font: inherit;
  color: var(--color-text-primary);
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: 6px;
  cursor: grab;
  transition: border-color var(--transition-fast, 0.12s), background var(--transition-fast, 0.12s);
}
.hwdev:not(.hwdev--multi) .hw { background: var(--color-surface); }
.hw:hover { border-color: var(--color-accent); }
.hw:active { cursor: grabbing; }
.hw:focus-visible { outline: none; box-shadow: 0 0 0 2px var(--color-accent); }
.hw--selected {
  border-color: var(--color-accent);
  background: color-mix(in srgb, var(--color-accent) 12%, var(--color-surface)) !important;
  box-shadow: 0 0 0 1px var(--color-accent);
}
.hw--dragging { opacity: 0.5; }
.hw__grip { font-size: 18px; color: var(--color-text-disabled); flex: 0 0 auto; }
.hw__text { display: flex; flex-direction: column; gap: 2px; min-width: 0; }
.hw__label {
  display: flex;
  align-items: center;
  gap: 6px;
  font-size: 13px;
  min-width: 0;
}
/* Device names are long and the tail ("… 404HD 192k") is often the part
   that tells two apart, so wrap rather than truncate. */
.hw__name { min-width: 0; overflow-wrap: anywhere; line-height: 1.3; }
.hw__used {
  font-size: 11px;
  color: var(--color-text-secondary);
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.hw__badge {
  flex: 0 0 auto;
  padding: 0 6px;
  font-size: 10px;
  font-weight: 600;
  letter-spacing: 0.03em;
  text-transform: uppercase;
  color: var(--color-text-secondary);
  border: 1px solid var(--color-border);
  border-radius: 999px;
}

/* ---------------- Advanced ---------------- */
.remap-advanced {
  display: flex;
  flex-direction: column;
  gap: 10px;
  padding-top: 12px;
  border-top: 1px solid var(--color-border);
}
.remap-advanced__toggle {
  display: flex;
  align-items: center;
  gap: 6px;
  padding: 2px 0;
  font: inherit;
  text-align: left;
  color: var(--color-text-primary);
  background: none;
  border: none;
  cursor: pointer;
}
.remap-advanced__toggle:focus-visible { outline: 2px solid var(--color-accent); outline-offset: 2px; border-radius: 4px; }
.remap-advanced__chev { font-size: 20px; color: var(--color-text-secondary); transition: transform var(--transition-fast, 0.12s); }
.remap-advanced__chev.is-open { transform: rotate(90deg); }
.remap-advanced__title { font-size: 13px; font-weight: 600; }
.remap-advanced__hint { font-size: 12px; color: var(--color-text-secondary); }

.outmap-section { display: flex; flex-direction: column; gap: 10px; }

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

/* The hardware this machine has, under the map that points at it. */
.outmap-devices { display: flex; flex-direction: column; gap: 8px; }
.outmap-devlist { list-style: none; margin: 0; padding: 0; display: flex; flex-direction: column; gap: 4px; }
.outmap-dev {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 6px 10px;
  background: var(--color-surface);
  border: 1px solid var(--color-border);
  border-radius: 6px;
  font-size: 12px;
}
/* The name is the only part that may grow, so a long interface name
   ellipsises instead of pushing the channel count and the button off the row. */
.outmap-dev__name {
  flex: 1 1 auto;
  min-width: 0;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  color: var(--color-text-primary);
}
.outmap-dev__count {
  flex: 0 0 auto;
  color: var(--color-text-secondary);
  font-variant-numeric: tabular-nums;
}

/* Right-aligned like the footer they replace, but inside the pane's flow
   rather than pinned to the bottom of a modal. */
.outmap-actions {
  display: flex;
  justify-content: flex-end;
  gap: 8px;
  margin-top: 4px;
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

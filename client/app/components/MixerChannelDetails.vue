<template>
  <!--
    The channel view: one bus, opened out across the whole mixer window.

    This is the layout a large-format live console uses for its channel screen,
    and the shape carries meaning. The channel column stands on the left at full
    height, with its own fader under your hand. Processing occupies the rest,
    all of it visible at once — an operator reaching for a compressor mid-show
    should not have to find a tab first. Channel selection runs along the
    bottom, where a console puts its select row, with arrows for stepping.

    Reading order across the work area: what shapes the sound (EQ, then
    dynamics beneath it), then what is inserted into it (plugins), then what is
    connected to it in either direction (contributions in, sends out).

    EQ, dynamics, filters and plugins are labelled shells until the DSP stage
    lands. They are laid out at full size rather than hidden so that stage fills
    panels instead of inventing navigation late.
  -->
  <div class="det">
    <header class="det__head">
      <div ref="colorWrapRef" class="det__colorwrap">
        <button
          class="det__chip"
          :style="{ background: bus.color || 'var(--color-accent)' }"
          :title="t('mixer.busColor')"
          @click.stop="colorPickerOpen = !colorPickerOpen"
        ></button>
        <div v-if="colorPickerOpen" class="det__colorpopover">
          <button
            v-for="color in PRESET_COLORS"
            :key="color"
            class="det__colorswatch"
            :style="{ background: color }"
            :class="{ 'det__colorswatch--active': bus.color === color }"
            :title="color"
            @click="onPickColor(color)"
          ></button>
        </div>
      </div>
      <div
        class="det__titlewrap"
        :title="bus.system ? '' : (renaming ? '' : bus.name + ' — ' + t('mixer.renameHint'))"
      >
        <input
          v-if="renaming"
          ref="nameInput"
          class="det__titleinput"
          :value="bus.name"
          @click.stop
          @keyup.enter="commitRename"
          @keyup.esc="renaming = false"
          @blur="commitRename"
        />
        <span v-else class="det__title" @dblclick="startRename">{{ bus.name }}</span>
      </div>
      <span class="det__meta">{{ widthLabel }} · {{ outputSummary }}</span>

      <div class="det__spacer"></div>
      <button
        v-if="!bus.system"
        class="det__delete"
        :title="t('mixer.deleteBus')"
        @click="$emit('delete', bus.id)"
      >
        <span class="material-symbols-rounded">delete</span>
      </button>
      <button class="det__close" :title="t('mixer.backToMixer')" @click="$emit('close')">
        <span class="material-symbols-rounded">close</span>
      </button>
    </header>

    <div class="det__main">
      <MixerChannelFader
        :bus="bus"
        :prev-id="prevId"
        :next-id="nextId"
        @patch="(id: string, p: Partial<Bus>) => $emit('patch', id, p)"
        @select="(id: string) => $emit('select', id)"
        @dsp-live="onDspLive"
      />

      <!-- Two columns. EQ takes the height on the left with the plugin rack
           tucked beneath it; dynamics takes the height on the right with the
           connection panels beneath. EQ and dynamics are side by side rather
           than stacked because stacking them made both too short to use. -->
      <div class="det__work">
        <!-- The curve needs the bus: it draws the channel's high- and low-pass
           alongside the EQ bands, and those live on the fader column. It gets
           the live-merged copy so it tracks a filter knob while it is moving,
           not 250 ms later when the value settles onto the bus. -->
      <MixerEqPanel
        class="det__eq"
        :bus="curveBus"
        @patch="(id: string, p: Partial<Bus>) => $emit('patch', id, p)"
        @dsp-live="onDspLive"
      />

        <!-- Static height, packed as tight as the slots allow: the rack is a
             list of six things, not a workspace, so it should never take room
             the EQ could use. -->
        <section class="det__panel det__panel--pending det__plugins">
          <h4 class="det__h">
            {{ t('mixer.plugins') }}
            <span class="det__pending">{{ t('mixer.notImplemented') }}</span>
          </h4>
          <div class="det__rack">
            <button v-for="n in 6" :key="n" class="det__insert" disabled>
              <span class="det__insertnum">{{ n }}</span>
              <span class="det__insertname">{{ t('mixer.emptySlot') }}</span>
            </button>
          </div>
        </section>

        <MixerDynamicsPanel
        class="det__dyn"
        :bus="curveBus"
        @patch="(id: string, p: Partial<Bus>) => $emit('patch', id, p)"
        @dsp-live="onDspLive"
      />

        <!-- What is connected, in signal order: what arrives at this bus, then
             where it goes. Sends takes the height it needs at the bottom and
             contributions takes the rest, scrolling its list inside itself. -->
        <div class="det__io">
          <section class="det__panel det__contrib">
            <h4 class="det__h">
              {{ t('mixer.feedingThis') }}
              <span class="det__count">{{ bus.itemUuids.length }}</span>
            </h4>
            <div class="det__contriblist">
              <ul v-if="bus.itemUuids.length" class="det__items">
                <li v-for="uuid in bus.itemUuids" :key="uuid">{{ itemName(uuid) }}</li>
              </ul>
              <p v-else class="det__none">{{ t('mixer.nothingAssigned') }}</p>
            </div>
          </section>

          <section class="det__panel">
            <h4 class="det__h">{{ t('mixer.sends') }}</h4>
            <label class="det__field">
              <span>{{ t('mixer.output') }}</span>
              <select :value="outputValue" @change="onOutputChange">
                <!-- The channel view never opens on Monitor — it is not in
                     userBuses (MixerPanel.vue) — so unlike the strip's select
                     this one needs no monitor guard on either option group. -->
                <option value="master">{{ t('mixer.toMaster') }}</option>
                <option v-for="o in outputOptions" :key="'out:' + o" :value="'out:' + o">{{ o }}</option>
                <!-- Bus targets: any other non-system bus, minus ones a route
                     here would loop back through. Convenience only — the
                     server's 409 is the real authority; see onOutputChange. -->
                <optgroup v-if="busOptions.length" :label="t('mixer.busesGroup')">
                  <option v-for="b in busOptions" :key="'bus:' + b.id" :value="'bus:' + b.id">{{ b.name }}</option>
                </optgroup>
              </select>
            </label>
            <label class="det__field">
              <span>{{ t('mixer.width') }}</span>
              <select
                :value="bus.width"
                :disabled="bus.system"
                @change="$emit('patch', bus.id, { width: Number(($event.target as HTMLSelectElement).value) })"
              >
                <option :value="1">{{ t('mixer.mono') }}</option>
                <option :value="2">{{ t('mixer.stereo') }}</option>
              </select>
            </label>
            <!-- Replaces the old dead-end warning text: this is now the D3
                 click affordance straight into the Remap Hardware Outputs
                 modal, since the select above can only pick a logical name,
                 never bind one to hardware. -->
            <button
              v-if="unmapped"
              class="det__warn det__warnbtn"
              @click="$emit('open-output-map')"
            >
              {{ t('mixer.outputUnmapped', { name: bus.output.target }) }}
            </button>
            <!-- A bus-kind route that never reaches the master: not a mapping
                 problem, so no click affordance, just the warning. -->
            <p v-if="busUnbound" class="det__warn">{{ t('mixer.busRouteUnbound') }}</p>
            <!-- Inline, non-blocking: the server's own 409 text for the last
                 rejected route, so the reason is legible without a dialog. -->
            <p v-if="outputErrorMsg" class="det__warn">{{ outputErrorMsg }}</p>
            <p class="det__none">{{ t('mixer.auxSendsPending') }}</p>
          </section>
        </div>
      </div>
    </div>

    <!-- Channel select row, where a console puts it. Each tile carries a live
         meter: the point of the row is knowing which channel to go to, and on
         a desk that judgement is made by watching level, not by reading names.
         No scale — at this size it would be unreadable — but the same meter
         component as everywhere else, so the colours mean the same thing. -->
    <footer class="det__bank">
      <button
        class="det__nav"
        :disabled="!prevId"
        :title="t('mixer.prevChannel')"
        @click="$emit('select', prevId)"
      >
        <span class="material-symbols-rounded">chevron_left</span>
      </button>

      <div class="det__banklist">
        <button
          v-for="b in buses"
          :key="b.id"
          class="det__mini"
          :class="{ 'det__mini--active': b.id === bus.id }"
          @click="$emit('select', b.id)"
        >
          <span class="det__minimeter">
            <StereoMeter
              v-if="b.mixerId"
              :mixer-id="b.mixerId"
              :mono="b.width < 2"
              bare
              :show-scale="false"
              :min-db="FADER_MIN_DB"
              :max-db="METER_MAX_DB"
            />
          </span>
          <span class="det__minitext">
            <span class="det__minichip" :style="{ background: b.color || 'var(--color-accent)' }"></span>
            <span class="det__mininame">{{ b.name }}</span>
          </span>
        </button>
      </div>

      <button
        class="det__nav"
        :disabled="!nextId"
        :title="t('mixer.nextChannel')"
        @click="$emit('select', nextId)"
      >
        <span class="material-symbols-rounded">chevron_right</span>
      </button>

      <!-- The mixer's own controls ride this bar rather than a title row of
           their own; the parent fills them in. -->
      <slot name="actions" />
    </footer>
  </div>
</template>

<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, ref, watch } from 'vue';
import type { Bus, BusDsp } from '~/types/project';
import { PRESET_COLORS } from '~/types/project';
import MixerChannelFader from './MixerChannelFader.vue';
import MixerEqPanel from './MixerEqPanel.vue';
import MixerDynamicsPanel from './MixerDynamicsPanel.vue';
import StereoMeter from './StereoMeter.vue';
import { FADER_MIN_DB, METER_MAX_DB } from '~/utils/meterScale';

const props = defineProps<{
  bus: Bus;
  buses: Bus[];
  outputNames: string[];
}>();

const emit = defineEmits<{
  (e: 'patch', id: string, patch: Partial<Bus>): void;
  (e: 'delete', id: string): void;
  (e: 'select', id: string): void;
  (e: 'close'): void;
  (e: 'open-output-map'): void;
}>();

const { t } = useLocalization();
const { findItemByUuid } = useProject();
const server = useLiveplayServer();

// The filter values as they are being dragged, ahead of the bus catching up.
//
// The fader column owns the knobs and the EQ panel draws the curve, so the
// in-flight value has to cross between them. It is held here, at their nearest
// common parent, rather than in shared module state — this is the only place
// that needs to know, and it clears itself when the channel changes.
const liveDsp = ref<Partial<BusDsp> | null>(null);
watch(() => props.bus?.id, () => {
  liveDsp.value = null;
  renaming.value = false;
  colorPickerOpen.value = false;
  outputErrorMsg.value = '';
});
// Once the settled value has landed on the bus, stop overriding with a stale
// copy of the same thing.
watch(() => props.bus?.dsp, () => { liveDsp.value = null; }, { deep: true });

// Merged rather than replaced: the filters come from the fader column and the
// EQ bands from the panel, and dragging one must not drop the other's
// in-flight value out of the curve.
function onDspLive(dsp: Partial<BusDsp>) {
  liveDsp.value = { ...(liveDsp.value ?? {}), ...dsp };
}

const curveBus = computed<Bus | null>(() => {
  if (!props.bus) return null;
  if (!liveDsp.value) return props.bus;
  return { ...props.bus, dsp: { ...props.bus.dsp, ...liveDsp.value } };
});

// Stepping order follows the rail, and includes buses the engine currently has
// no strip for — they are still channels, and skipping them would make the
// arrows land somewhere other than the next tile in the row below.
const index  = computed(() => props.buses.findIndex(b => b.id === props.bus.id));
const prevId = computed(() => props.buses[index.value - 1]?.id ?? '');
const nextId = computed(() => props.buses[index.value + 1]?.id ?? '');

const widthLabel = computed(() =>
  props.bus.width >= 2 ? t('mixer.stereo') : t('mixer.mono'));

const outputValue = computed(() => {
  const o = props.bus.output;
  if (o.type === 'bus') return 'bus:' + o.target;
  if (o.type === 'output') return 'out:' + o.target;
  return 'master';
});

// For a bus target, the summary reads the target bus's own name rather than
// its id — "→ Beds", not "→ beds". Falls back to the raw id if the target
// bus is somehow not in the loaded list (e.g. a stale broadcast mid-delete).
const outputSummary = computed(() => {
  const o = props.bus.output;
  if (o.type === 'master') return t('mixer.toMaster');
  if (o.type === 'bus') {
    const target = props.buses.find(b => b.id === o.target);
    return target?.name ?? o.target;
  }
  return o.target;
});

const outputOptions = computed(() => {
  const names = [...props.outputNames];
  const target = props.bus.output.target;
  if (props.bus.output.type === 'output' && target && !names.includes(target)) {
    names.push(target);
  }
  return names;
});

// Candidate bus targets, same rule as the strip's (MixerStrip.vue): any other
// non-system bus, excluding this one and excluding any bus whose existing
// output chain already reaches this one. Convenience only — the server 409
// is the actual authority.
function reachesBus(fromId: string, toId: string, all: Bus[]): boolean {
  const visited = new Set<string>();
  let cur = all.find(b => b.id === fromId);
  while (cur && cur.output.type === 'bus') {
    const nextId = cur.output.target;
    if (nextId === toId) return true;
    if (visited.has(nextId)) break;
    visited.add(nextId);
    cur = all.find(b => b.id === nextId);
  }
  return false;
}
const busOptions = computed(() => {
  // props.buses is already the non-system list (MixerPanel's userBuses); a
  // 'bus' type target can never be a system bus in the first place, so it is
  // also everything a chain could walk through.
  const all = props.buses;
  return all.filter(b =>
    b.id !== props.bus.id && !reachesBus(b.id, props.bus.id, all));
});

const unmapped = computed(() =>
  props.bus.output.type === 'output' && props.bus.bound === false);

// A bus-kind route that never reaches the master, per D10 — bound is
// server-computed and just rendered here, not re-derived.
const busUnbound = computed(() =>
  props.bus.output.type === 'bus' && props.bus.bound === false);

// Transient: the server's 409 text for the last rejected route, cleared on
// the next successful change or after a few seconds.
const outputErrorMsg = ref('');
let outputErrorTimer: ReturnType<typeof setTimeout> | null = null;

function serverErrorText(err: unknown): string {
  const msg = err instanceof Error ? err.message : String(err);
  const body = msg.slice(msg.indexOf('—') + 1).trim();
  try {
    const parsed = JSON.parse(body);
    if (parsed && typeof parsed.error === 'string') return parsed.error;
  } catch { /* not JSON; fall through */ }
  return body || msg;
}

// Goes straight to the server, same as the strip's version, rather than
// through emit('patch') — that path is fire-and-forget and can't catch a 409
// to revert the select and show the reason.
async function onOutputChange(e: Event) {
  const el = e.target as HTMLSelectElement;
  const v = el.value;
  const prevValue = outputValue.value;
  const patch = v === 'master'
    ? { output: { type: 'master' as const, target: '' } }
    : v.startsWith('bus:')
      ? { output: { type: 'bus' as const, target: v.slice(4) } }
      : { output: { type: 'output' as const, target: v.slice(4) } };
  try {
    await server.patchBus(props.bus.id, patch);
    outputErrorMsg.value = '';
  } catch (err) {
    // Vue won't force the DOM element back on its own here — outputValue's
    // own bound value hasn't changed, only the browser's live selection has
    // — so it is reset directly.
    el.value = prevValue;
    outputErrorMsg.value = serverErrorText(err);
    if (outputErrorTimer) clearTimeout(outputErrorTimer);
    outputErrorTimer = setTimeout(() => { outputErrorMsg.value = ''; }, 6000);
  }
}

function itemName(uuid: string): string {
  const it = findItemByUuid?.(uuid) as any;
  return it?.displayName || uuid;
}

// Inline rename, same pattern as the strip's scribble strip
// (MixerStrip.vue): system buses (Main, Monitor) keep their given names.
const renaming  = ref(false);
const nameInput = ref<HTMLInputElement | null>(null);

async function startRename() {
  if (props.bus.system) return;
  renaming.value = true;
  await nextTick();
  nameInput.value?.select();
}
function commitRename() {
  if (!renaming.value) return;
  renaming.value = false;
  const next = nameInput.value?.value?.trim();
  if (next && next !== props.bus.name) emit('patch', props.bus.id, { name: next });
}

// Colour swatch popover. Picking a colour patches the bus straight away —
// there is no drag gesture to debounce here, unlike gain/pan — and the
// server's buses_patched broadcast is what every window, including this one,
// renders from.
const colorPickerOpen = ref(false);
const colorWrapRef = ref<HTMLElement | null>(null);

function onDocClick(e: MouseEvent) {
  if (!colorPickerOpen.value) return;
  const target = e.target as Node;
  if (colorWrapRef.value && !colorWrapRef.value.contains(target)) {
    colorPickerOpen.value = false;
  }
}
if (typeof window !== 'undefined') {
  window.addEventListener('mousedown', onDocClick, true);
  onBeforeUnmount(() => window.removeEventListener('mousedown', onDocClick, true));
}
onBeforeUnmount(() => { if (outputErrorTimer) clearTimeout(outputErrorTimer); });

function onPickColor(color: string) {
  emit('patch', props.bus.id, { color });
  colorPickerOpen.value = false;
}
</script>

<style scoped>
.det {
  display: flex;
  flex-direction: column;
  flex: 1;
  min-height: 0;
  min-width: 0;
  background: var(--color-background);
}

.det__head {
  display: flex;
  align-items: center;
  gap: var(--spacing-xs);
  flex: 0 0 auto;
  padding: var(--spacing-xs) var(--spacing-sm);
  border-bottom: 1px solid var(--color-border);
}
.det__colorwrap { position: relative; flex: 0 0 auto; }
.det__chip {
  width: 10px;
  height: 10px;
  padding: 0;
  border: none;
  border-radius: 50%;
  flex: 0 0 auto;
  cursor: pointer;
}
.det__colorpopover {
  position: absolute;
  top: calc(100% + 4px);
  left: 0;
  z-index: 10;
  display: grid;
  grid-template-columns: repeat(8, 1fr);
  gap: 4px;
  padding: var(--spacing-xs);
  background: var(--color-surface);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-md);
  box-shadow: 0 4px 12px rgba(0, 0, 0, 0.3);
}
.det__colorswatch {
  width: 16px;
  height: 16px;
  padding: 0;
  border-radius: var(--border-radius-sm);
  border: 2px solid transparent;
  cursor: pointer;
}
.det__colorswatch:hover { transform: scale(1.1); }
.det__colorswatch--active {
  border-color: var(--color-text-primary);
  box-shadow: 0 0 0 2px var(--color-background);
}
/* The name is editable here, double-click as on the strip; system buses keep
   their given name, same restriction as the strip's scribble strip. */
.det__titlewrap { display: flex; align-items: center; min-width: 0; }
.det__title { font-size: 13px; color: var(--color-text-primary); cursor: text; }
.det__titleinput {
  font-size: 13px;
  color: var(--color-text-primary);
  background: var(--color-background);
  border: 1px solid var(--color-accent);
  border-radius: var(--border-radius-sm);
  padding: 1px 4px;
  min-width: 0;
}
.det__meta { font-size: 11px; color: var(--color-text-disabled); }
.det__delete, .det__close {
  display: flex;
  color: var(--color-text-secondary);
  background: none;
  border: none;
  cursor: pointer;
}
.det__delete:hover, .det__close:hover { color: var(--color-text-primary); }
.det__spacer { flex: 1; }

.det__main { display: flex; flex: 1; min-height: 0; min-width: 0; }

/* Three columns. EQ and the connection panels each own a full-height column;
   dynamics and the plugin rack share the middle one.

   The rows are 1fr over auto, which is what makes that work: the rack takes
   only the height its six slots need, dynamics absorbs the slack above it, and
   the two full-height columns span both so they fill the window. Sizing the
   rack row instead would hand the leftover height to six empty slots. */
.det__work {
  display: grid;
  grid-template-columns: minmax(260px, 1fr) minmax(320px, 1.05fr) minmax(220px, 0.75fr);
  grid-template-rows: minmax(0, 1fr) auto;
  gap: var(--spacing-sm);
  flex: 1;
  min-width: 0;
  padding: var(--spacing-sm);
  overflow: auto;
}
.det__eq      { grid-column: 1; grid-row: 1 / span 2; }
.det__dyn     { grid-column: 2; grid-row: 1; }
.det__plugins { grid-column: 2; grid-row: 2; }
.det__io      { grid-column: 3; grid-row: 1 / span 2; }

/* Narrow: one column. Everything takes the height it needs and the work area
   scrolls, which is where the EQ graph gets its full height back rather than
   being squeezed into a fraction of a small window. */
@media (max-width: 1180px) {
  .det__work {
    grid-template-columns: 1fr;
    grid-template-rows: none;
    grid-auto-rows: min-content;
  }
  .det__eq, .det__dyn, .det__plugins, .det__io {
    grid-column: 1;
    grid-row: auto;
  }
}

/* Short: stop dividing the height and let each row take what it needs, with
   the work area scrolling. The columns stay — a short window is usually a wide
   one, and collapsing to one column there wastes the width it does have.

   Only grid-template-rows changes. An earlier version also set grid-row: auto
   while leaving the explicit columns in place, which handed placement back to
   auto-flow: the cursor never moves backwards, so after EQ and plugins filled
   column 1 rows 1 and 2, dynamics landed in column 2 *row 2* beside plugins
   instead of at the top, with the connection panels pushed to row 3. */
@media (max-height: 660px) {
  .det__work { grid-template-rows: auto auto; }
}

.det__panel {
  display: flex;
  flex-direction: column;
  gap: var(--spacing-xs);
  padding: var(--spacing-sm);
  background: var(--color-surface);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-md);
  min-height: 0;
}
/* Shells read as unfinished on purpose — a dashed edge and a label, so nothing
   here can be mistaken for a control that does something. */
.det__panel--pending { border-style: dashed; }

/* The `det__` classes are a vocabulary shared with the child panels, so they
   have to be written to cross the scope boundary.

   Scoped CSS compiles `.det__h` to `.det__h[data-v-thisfile]`, and Vue only
   stamps this file's id onto a child component's ROOT element. `.det__panel`
   worked by luck, because it happens to be each panel's root; `.det__h` never
   applied to the headers inside MixerEqPanel or MixerDynamicsPanel at all,
   which is why they have been rendering as bare h4s. Pairing each selector
   with its :deep() form covers this component's own elements and the
   children's alike. */
.det__byp,
:deep(.det__byp) {
  margin-left: auto;
  padding: 1px 5px;
  font-size: 9px;
  font-family: var(--font-mono);
  letter-spacing: 0.06em;
  color: var(--color-text-secondary);
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-sm);
  cursor: pointer;
}
.det__byp:hover:not(:disabled),
:deep(.det__byp:hover:not(:disabled)) { color: var(--color-text-primary); }
.det__byp:disabled,
:deep(.det__byp:disabled) { opacity: 0.4; cursor: not-allowed; }
/* Lit means OUT of circuit. A bypass lamp marks the abnormal state, which is
   the one worth spotting from a distance. */
.det__byp--on,
:deep(.det__byp--on) {
  color: #fff;
  background: var(--color-warning, #b28600);
  border-color: var(--color-warning, #b28600);
}

.det__h,
:deep(.det__h) {
  display: flex;
  align-items: center;
  gap: var(--spacing-xs);
  margin: 0;
  font-size: 10px;
  letter-spacing: 0.08em;
  text-transform: uppercase;
  color: var(--color-text-secondary);
}
.det__pending {
  font-size: 9px;
  letter-spacing: 0.04em;
  text-transform: none;
  color: var(--color-text-disabled);
}

/* Three across, two down, and no taller than the slots need. The rack sits in
   the content-sized row precisely so it stays compact — height it took would
   come out of dynamics above it, and six empty slots are not worth that. */
.det__plugins { min-height: 0; }
.det__rack {
  display: grid;
  grid-template-columns: repeat(3, 1fr);
  gap: 4px;
}
.det__insert {
  display: flex;
  align-items: center;
  justify-content: flex-start;
  gap: 5px;
  min-width: 0;
  min-height: 26px;
  font-size: 11px;
  padding: 5px 6px;
  color: var(--color-text-disabled);
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-sm);
  cursor: not-allowed;
}
.det__insertnum { font-family: var(--font-mono); font-size: 9px; opacity: 0.6; flex: 0 0 auto; }
.det__insertname { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }

.det__field {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 6px;
  font-size: 11px;
  color: var(--color-text-secondary);
}
.det__field select {
  font-size: 11px;
  padding: 3px;
  color: var(--color-text-primary);
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-sm);
}

.det__warn { margin: 0; font-size: 11px; color: var(--color-warning); }
/* The unmapped warning is a click affordance into the remap modal, not just
   a status line, so it reads as text but behaves as a button. */
.det__warnbtn {
  text-align: left;
  background: none;
  border: none;
  padding: 0;
  cursor: pointer;
  text-decoration: underline;
  text-decoration-style: dotted;
}
.det__warnbtn:hover { color: var(--color-danger, var(--color-warning)); }
.det__none { list-style: none; margin: 0; font-size: 11px; color: var(--color-text-disabled); }

/* Contributions above, sends below, in one full-height column.
   Sends has fixed content and takes only what it needs; contributions takes
   the rest and scrolls its list inside itself. A bus can feed a hundred cues,
   so the list must never be what decides the column's height — the count in
   the heading is there to tell you the length without scrolling to find it. */
.det__io {
  display: flex;
  flex-direction: column;
  gap: var(--spacing-sm);
  min-height: 0;
}
.det__contrib { flex: 1 1 auto; min-height: 90px; }
.det__io > .det__panel:not(.det__contrib) { flex: 0 0 auto; }
.det__count {
  margin-left: auto;
  font-family: var(--font-mono);
  font-size: 10px;
  padding: 0 5px;
  color: var(--color-text-secondary);
  background: var(--color-background);
  border-radius: 8px;
}
.det__contriblist {
  flex: 1 1 auto;
  min-height: 0;
  overflow-y: auto;
}
.det__items {
  margin: 0;
  padding-left: 16px;
  font-size: 11px;
  color: var(--color-text-primary);
}

.det__bank {
  display: flex;
  align-items: stretch;
  gap: var(--spacing-xs);
  flex: 0 0 auto;
  padding: var(--spacing-xs) var(--spacing-sm);
  border-top: 1px solid var(--color-border);
  background: var(--color-surface);
}
.det__banklist { display: flex; gap: 3px; flex: 1; min-width: 0; overflow-x: auto; }
.det__nav {
  display: flex;
  align-items: center;
  flex: 0 0 auto;
  padding: 4px;
  color: var(--color-text-secondary);
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-sm);
  cursor: pointer;
}
.det__nav:disabled { opacity: 0.3; cursor: default; }

.det__mini {
  display: flex;
  align-items: center;
  gap: 5px;
  padding: 4px 8px 4px 5px;
  color: var(--color-text-secondary);
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-sm);
  cursor: pointer;
  white-space: nowrap;
}
.det__mini--active { border-color: var(--color-accent); color: var(--color-text-primary); }
/* Tall enough for a meter to be worth reading, short enough that the row stays
   a row. */
.det__minimeter { display: flex; height: 30px; flex: 0 0 auto; }
.det__minitext { display: flex; align-items: center; gap: 4px; font-size: 10px; }
.det__minichip { width: 6px; height: 6px; border-radius: 50%; flex: 0 0 auto; }
</style>

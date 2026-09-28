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

    A panel with nothing behind it yet is laid out at full size rather than
    hidden, so the stage that fills it fills a panel instead of inventing
    navigation late. The plugin rack is the exception and the reason the rule
    has one: inserts are far enough off that six disabled slots read as a
    feature the operator has failed to find. See SHOW_PLUGIN_RACK.
  -->
  <div class="det">
    <header class="det__head" @contextmenu.prevent="openMenuAt($event.clientX, $event.clientY)">
      <BusColorPicker :color="bus.color" size="lg" @pick="onPickColor" />
      <div
        class="det__titlewrap"
        :title="renaming ? '' : bus.name + ' — ' + t('mixer.renameHint')"
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
      <!-- The role badge (D24): the same mark the strip carries, so the
           channel view says which bus is the house and which is the phones
           without the operator having to go back to the rail to check. -->
      <span v-if="bus.master" class="det__badge det__badge--master">{{ t('mixer.roleMaster') }}</span>
      <span v-else-if="bus.preview" class="det__badge det__badge--preview">{{ t('mixer.rolePreview') }}</span>
      <span class="det__meta">{{ widthLabel }} · {{ outputSummary }}</span>

      <div class="det__spacer"></div>
      <!-- Delete is kept visible but disabled on a role holder, with the
           reason in the tooltip — the role has to move first (D24). The
           same menu as the strip's ⋮ sits beside it, which is where the
           role moves from. -->
      <button
        class="det__delete"
        :disabled="bus.master || bus.preview"
        :title="bus.master || bus.preview
          ? t('mixer.roleDeleteBlocked', { role: bus.master ? t('mixer.roleMaster') : t('mixer.rolePreview') })
          : t('mixer.deleteBus')"
        @click="$emit('delete', bus.id)"
      >
        <span class="material-symbols-rounded">delete</span>
      </button>
      <button ref="menuBtn" class="det__menu" :title="t('mixer.busMenu')" @click.stop="openMenuFromButton">
        <span class="material-symbols-rounded">more_vert</span>
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

      <!-- Three columns: EQ, then dynamics with the plugin rack tucked beneath
           it when there is one, then the connection panels. EQ and dynamics are
           side by side rather than stacked because stacking them made both too
           short to use. -->
      <div class="det__work" :class="{ 'det__work--norack': !SHOW_PLUGIN_RACK }">
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
             the EQ could use.

             HIDDEN until inserts actually exist — see SHOW_PLUGIN_RACK. Kept
             rather than deleted so the markup, the CSS and the three locale
             keys stay in step with each other instead of rotting apart while
             nothing renders them. -->
        <section v-if="SHOW_PLUGIN_RACK" class="det__panel det__panel--pending det__plugins">
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
            <!-- A mini playlist of what feeds the bus: numbers, colours,
                 groups and waveforms, as the operator knows the cues. -->
            <div class="det__contriblist">
              <MixerFeedingList :bus="bus" />
            </div>
          </section>

          <section class="det__panel">
            <h4 class="det__h">{{ t('mixer.sends') }}</h4>
            <label class="det__field">
              <span>{{ t('mixer.output') }}</span>
              <!-- The same picker as the strip's (BusOutputSelect.vue): buses,
                   outputs with their mappings, devices, and the action row
                   into the output map. It reverts itself on a 409 and hands
                   the reason up, shown inline below. -->
              <BusOutputSelect
                :bus="bus"
                :buses="buses"
                :outputs="outputs"
                @open-output-map="$emit('open-output-map')"
                @error="outputErrorMsg = $event"
              />
            </label>
            <!-- Width is a setup decision and belongs here rather than on the
                 strip; every bus has it, the role holders included. -->
            <label class="det__field">
              <span>{{ t('mixer.width') }}</span>
              <select
                :value="bus.width"
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
              {{ bus.preview ? t('mixer.previewUnmapped') : t('mixer.outputUnmapped', { name: bus.output.target }) }}
            </button>
            <!-- A bus-kind route that never reaches hardware: not a mapping
                 problem, so no click affordance, just the warning. -->
            <p v-if="busUnbound" class="det__warn">{{ t('mixer.busRouteUnbound') }}</p>
            <!-- Chose "no output" and has no send reaching one either. -->
            <p v-if="noneSilent" class="det__warn">{{ t('mixer.outputNoneSilent') }}</p>
            <!-- Inline, non-blocking: the server's own 409 text for the last
                 rejected route, so the reason is legible without a dialog. -->
            <p v-if="outputErrorMsg" class="det__warn">{{ outputErrorMsg }}</p>

            <!-- Aux sends (M1), under the output because that is signal order:
                 where the whole thing goes, then the copies taken off it. -->
            <h5 class="det__sub">{{ t('mixer.auxSends') }}</h5>
            <BusSendList :bus="bus" :buses="buses" />
          </section>
        </div>
      </div>
    </div>

    <!-- Channel select row, where a console puts it. Each tile carries a live
         meter: the point of the row is knowing which channel to go to, and on
         a desk that judgement is made by watching level, not by reading names.
         No scale — at this size it would be unreadable — but the same meter
         component as everywhere else, so the colours mean the same thing. -->
    <footer class="det__bank" :class="{ 'det__bank--mini': miniMixer }">
      <!-- Compact channel buttons, or the whole desk in miniature under the
           channel being edited (a per-viewer choice, remembered). -->
      <button
        class="det__minitoggle"
        :class="{ 'det__minitoggle--on': miniMixer }"
        :title="t('mixer.miniMixerHint')"
        @click="toggleMiniMixer"
      >
        <span class="material-symbols-rounded">view_week</span>
      </button>
      <button
        class="det__nav"
        :disabled="!prevId"
        :title="t('mixer.prevChannel')"
        @click="$emit('select', prevId)"
      >
        <span class="material-symbols-rounded">chevron_left</span>
      </button>

      <MixerMiniRail
        v-if="miniMixer"
        :buses="buses"
        :active-id="bus.id"
        @select="(id: string) => $emit('select', id)"
      />
      <div v-else class="det__banklist">
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
    </footer>

    <BusMenu
      :bus="bus"
      :open="menuOpen"
      :x="menuPos.x"
      :y="menuPos.y"
      @close="menuOpen = false"
      @rename="startRename"
      @color="onPickColor"
      @open="menuOpen = false"
      @set-master="$emit('set-role', bus.id, 'master')"
      @set-preview="$emit('set-role', bus.id, 'preview')"
      @delete="$emit('delete', bus.id)"
    />
  </div>
</template>

<script setup lang="ts">
import MixerMiniRail from './MixerMiniRail.vue';
import { computed, nextTick, ref, watch } from 'vue';
import type { Bus, BusDsp } from '~/types/project';
import type { OutputMap } from '~/composables/useLiveplayServer';
import MixerChannelFader from './MixerChannelFader.vue';
import MixerEqPanel from './MixerEqPanel.vue';
import MixerDynamicsPanel from './MixerDynamicsPanel.vue';
import StereoMeter from './StereoMeter.vue';
import BusOutputSelect from './BusOutputSelect.vue';
import BusSendList from './BusSendList.vue';
import BusColorPicker from './BusColorPicker.vue';
import BusMenu from './BusMenu.vue';
import { FADER_MIN_DB, METER_MAX_DB } from '~/utils/meterScale';

// The insert rack is not built, and six disabled slots labelled "not
// implemented" are a promise the mixer cannot keep — an operator reads them as
// a feature they have failed to find rather than one that does not exist. Flip
// this to true in the same change that gives the slots something to hold.
const SHOW_PLUGIN_RACK = false;

const props = defineProps<{
  bus: Bus;
  /** Every bus, rail order then the pinned pair — the stepping order. */
  buses: Bus[];
  /** The machine's output map, null until fetched. */
  outputs: OutputMap | null;
}>();

// The mini mixer along the bottom: on by default, remembered per viewer
// (how this operator likes to look, not a property of the show).
const MINI_KEY = 'liveplay.channelMiniMixer';
const miniMixer = ref(true);
try { const v = localStorage.getItem(MINI_KEY); if (v === '0') miniMixer.value = false; } catch { /* no storage */ }
function toggleMiniMixer() {
  miniMixer.value = !miniMixer.value;
  try { localStorage.setItem(MINI_KEY, miniMixer.value ? '1' : '0'); } catch { /* no storage */ }
}

const emit = defineEmits<{
  (e: 'patch', id: string, patch: Partial<Bus>): void;
  (e: 'delete', id: string): void;
  (e: 'set-role', id: string, role: 'master' | 'preview'): void;
  (e: 'select', id: string): void;
  (e: 'close'): void;
  (e: 'open-output-map'): void;
}>();

const { t } = useLocalization();
const { findItemByUuid } = useProject();

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
  menuOpen.value = false;
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

// The output summary in the header: "→ <target>" for a hardware route, the
// target bus's own name for a bus route — "→ Beds", not "→ beds". Falls back
// to the raw id if the target bus is somehow not in the loaded list (a stale
// broadcast mid-delete).
const outputSummary = computed(() => {
  const o = props.bus.output;
  if (o.type === 'bus') {
    const target = props.buses.find(b => b.id === o.target);
    return '→ ' + (target?.name ?? o.target);
  }
  return '→ ' + o.target;
});

const unmapped = computed(() =>
  props.bus.output.type === 'output' && props.bus.bound === false);

// A bus-kind route that never reaches hardware, per D10 — bound is
// server-computed and just rendered here, not re-derived.
const busUnbound = computed(() =>
  props.bus.output.type === 'bus' && props.bus.bound === false);

// No output AND no send that reaches one. Its own warning rather than
// busRouteUnbound's, which talks about a route this bus does not have — the
// operator chose "no output" deliberately, and what they need to be told is
// that the sends are not carrying it either.
const noneSilent = computed(() =>
  props.bus.output.type === 'none' && props.bus.bound === false);

// The server's 409 text for the last rejected route, handed up by the picker
// and shown inline; it clears itself.
const outputErrorMsg = ref('');

function itemName(uuid: string): string {
  const it = findItemByUuid?.(uuid) as any;
  return it?.displayName || uuid;
}

// Inline rename, same pattern as the strip's scribble strip
// (MixerStrip.vue). Every bus can be renamed, role holders included.
const renaming  = ref(false);
const nameInput = ref<HTMLInputElement | null>(null);

async function startRename() {
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

// Picking a colour patches the bus straight away — there is no drag gesture
// to debounce here, unlike gain/pan — and the server's buses_patched
// broadcast is what every window, including this one, renders from.
function onPickColor(color: string) {
  emit('patch', props.bus.id, { color });
}

// The ⋮ menu — the same BusMenu the strip uses, from the header button or a
// right-click on the header. Its "Channel settings…" entry is a no-op here,
// since this is the channel settings.
const menuOpen = ref(false);
const menuPos  = ref({ x: 0, y: 0 });
const menuBtn  = ref<HTMLButtonElement | null>(null);

function openMenuAt(x: number, y: number) {
  menuPos.value = { x, y };
  menuOpen.value = true;
}
function openMenuFromButton() {
  const r = menuBtn.value?.getBoundingClientRect();
  if (r) openMenuAt(r.left, r.bottom + 2);
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
/* The name is editable here, double-click as on the strip. */
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
/* The role badge, the same mark as the strip's (MixerStrip.vue .strip__badge):
   Master in the accent, Preview in PFL's green. */
.det__badge {
  padding: 0 4px;
  font-family: var(--font-mono);
  font-size: 9px;
  font-weight: 600;
  letter-spacing: 0.08em;
  text-transform: uppercase;
  line-height: 13px;
  border-radius: 2px;
  color: #fff;
  flex: 0 0 auto;
}
.det__badge--master  { background: var(--color-accent); }
.det__badge--preview { background: var(--color-success); }
.det__delete, .det__menu, .det__close {
  display: flex;
  color: var(--color-text-secondary);
  background: none;
  border: none;
  cursor: pointer;
}
.det__delete:hover:not(:disabled), .det__menu:hover, .det__close:hover { color: var(--color-text-primary); }
.det__delete:disabled { opacity: 0.35; cursor: not-allowed; }
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
  grid-template-columns: minmax(300px, 1.4fr) minmax(300px, 1fr) minmax(240px, 0.8fr);
  grid-template-rows: minmax(0, 1fr) auto;
  gap: var(--spacing-sm);
  flex: 1;
  min-width: 0;
  padding: var(--spacing-sm);
  overflow: auto;
}
/* min-width:0 AND min-height:0 on every one of them is load-bearing, not
   tidiness. A grid item defaults to `min-width: auto` / `min-height: auto`,
   meaning it will NOT shrink below its own min-content size on that axis — it
   overflows its track instead, and an overflowing track paints straight over
   whatever is next to it. .det__work already had min-width:0; the items inside
   it never did, and the container was never what overflowed.

   Both axes matter and they were two separate bugs. Width: the dynamics
   controls landing on "Feeding this bus" in the column beside them. Height:
   .det__dyn sits in row 1 and .det__plugins in row 2, so a dynamics panel that
   would not shrink into its row simply grew down through the plugin rack
   underneath it. Fixing only the inline axis left the block axis doing exactly
   the same thing one direction over. */
.det__eq      { grid-column: 1; grid-row: 1 / span 2; min-width: 0; min-height: 0; }
.det__dyn     { grid-column: 2; grid-row: 1;          min-width: 0; min-height: 0; }
.det__plugins { grid-column: 2; grid-row: 2;          min-width: 0; min-height: 0; }
.det__io      { grid-column: 3; grid-row: 1 / span 2; min-width: 0; min-height: 0; }

/* With the rack hidden there is no second row to hold, so the grid drops to
   one. Leaving the two-row template in place would collapse row 2 to nothing
   but still lay its gap, and dynamics — the only item not spanning both rows —
   would finish 8px above the columns either side of it. */
.det__work--norack { grid-template-rows: minmax(0, 1fr); }
.det__work--norack .det__eq,
.det__work--norack .det__dyn,
.det__work--norack .det__io { grid-row: 1; }

/* Narrow: one column. Everything takes the height it needs and the work area
   scrolls, which is where the EQ graph gets its full height back rather than
   being squeezed into a fraction of a small window.

   AGAINST THE MIXER, NOT THE VIEWPORT. This was a viewport `@media` query, and
   the mixer is not the viewport: docked, it is a flex child whose width comes
   from a splitter, so a 1400px window with the mixer docked at 700px kept the
   three-column rule while the container sat far below what it needs. The
   columns have `minmax()` floors, and a floor overflows rather than shrinks —
   which is what put the controls on top of each other. MixerPanel.vue already
   declares `container: mixer / inline-size` and says why in its own comment;
   MixerActions.vue already queries it. This view simply never followed the
   rule the panel around it had already established.

   1040px is the measured floor, not a guess: the fader column is 148px
   (border-box), and .det__work needs 16px padding + 2 × 8px gap + the three
   column floors of 300 + 300 + 240 = 872px. That is 1020px, plus ~16px so the
   work area's own scrollbar cannot push it back over the edge. */
@container mixer (max-width: 1040px) {
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
   instead of at the top, with the connection panels pushed to row 3.

   This one stays a viewport @media query, deliberately. Height is the axis
   where the mixer really does track the window: docked it takes the full
   workspace height, and detached it is the window. Width is the axis a
   splitter can change independently, which is why only that query moved to the
   container. Making this a container query too would mean `container-type:
   size` on .mixer, which adds block-axis containment the panel does not need
   and MixerActions' inline-size query never asked for. */
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

/* A second-level heading inside a panel, for the sends list under the output.
   Quieter than .det__h: the panel already has one heading, and this divides it
   rather than introducing a new section. */
.det__sub {
  margin: 4px 0 0;
  font-size: 10px;
  font-weight: 600;
  letter-spacing: 0.05em;
  text-transform: uppercase;
  color: var(--color-text-secondary);
}

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
.det__bank--mini { height: clamp(170px, 24vh, 260px); align-items: stretch; }
.det__minitoggle {
  display: grid;
  place-items: center;
  flex: 0 0 auto;
  width: 26px;
  height: 26px;
  align-self: flex-start;
  color: var(--color-text-secondary);
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-sm);
  cursor: pointer;
}
.det__minitoggle--on { color: #fff; background: var(--color-accent); border-color: var(--color-accent); }
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

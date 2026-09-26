<template>
  <!--
    "Feeding this bus": the cues that play through this bus, drawn as a small
    playlist rather than a list of names, so they can be recognised the way
    the operator already knows them — by colour, by number, by the shape of
    the waveform, and by which group they sit in.

    The server hands over a flat list (bus.itemUuids: audio items only, in
    document order, groups never included). The tree is rebuilt here from the
    playlist, keeping only the branches that lead to one of those items, so
    a group row appears exactly when something inside it feeds this bus.
    Anything that is not in the playlist tree is a cart cue and goes in a
    section of its own at the end.

    Cheap by construction: one IntersectionObserver and one ResizeObserver for
    the whole list rather than one of each per row, and a canvas is drawn only
    once it is near the visible part of the list.
  -->
  <div ref="root" class="feed">
    <p v-if="!rows.length" class="feed__none">{{ t('mixer.nothingAssigned') }}</p>

    <template v-for="row in rows" :key="row.key">
      <div v-if="row.kind === 'section'" class="feed__section">
        <span class="material-symbols-rounded">grid_view</span>
        {{ t('mixer.feeding.cart') }}
      </div>

      <div
        v-else-if="row.kind === 'group'"
        class="feed__row feed__row--group"
        :class="{ 'is-playing': groupLive(row.group.uuid), 'is-selected': selectedItems.has(row.group.uuid) }"
        :style="rowStyle(row.group.color, row.depth, !!groupLive(row.group.uuid))"
        role="button"
        tabindex="0"
        :title="t('mixer.feeding.selectHint')"
        @click="select(row.group.uuid)"
        @keydown.enter="select(row.group.uuid)"
      >
        <span class="feed__stripe" :style="{ background: row.group.color }"></span>
        <span class="feed__index">{{ formatItemIndex(row.group.index) }}</span>
        <span class="material-symbols-rounded feed__folder">folder</span>
        <span class="feed__name">{{ row.group.displayName }}</span>
        <span
          v-if="row.feeding < row.total"
          class="feed__partial"
          :title="t('mixer.feeding.partial', { n: row.feeding, total: row.total })"
        >{{ row.feeding }}/{{ row.total }}</span>
        <span
          v-if="row.group.busId === bus.id"
          class="feed__route feed__route--set"
          :style="{ background: bus.color || 'var(--color-accent)' }"
          :title="t('mixer.feeding.groupSets')"
        ></span>
        <span class="feed__time" :title="groupTimeTitle(row)">{{ groupTime(row) }}</span>
      </div>

      <div
        v-else
        class="feed__row feed__row--audio"
        :class="{
          'is-playing': activeCues.has(row.item.uuid),
          'is-selected': selectedItems.has(row.item.uuid),
          'is-cart': row.cart,
        }"
        :style="rowStyle(row.item.color, row.depth, activeCues.has(row.item.uuid))"
        :role="row.cart ? undefined : 'button'"
        :tabindex="row.cart ? undefined : 0"
        :title="row.cart ? undefined : t('mixer.feeding.selectHint')"
        @click="!row.cart && select(row.item.uuid)"
        @keydown.enter="!row.cart && select(row.item.uuid)"
      >
        <canvas :ref="canvasRef(row.item.uuid)" class="feed__wave"></canvas>
        <span
          v-if="activeCues.has(row.item.uuid)"
          class="feed__progress"
          :style="{ width: progress(row.item.uuid) + '%', background: row.item.color }"
        ></span>
        <span class="feed__stripe" :style="{ background: row.item.color }"></span>
        <span class="feed__index">{{ row.cart ? (row.slot !== null ? row.slot + 1 : '–') : formatItemIndex(row.item.index) }}</span>
        <span class="feed__name">{{ row.item.displayName || row.item.uuid }}</span>
        <!-- How the cue gets here: a filled dot in the bus colour when the
             cue names this bus itself (the playlist's D19 dot), a faint
             inherit arrow when it arrives by way of a group or by default. -->
        <span
          v-if="row.explicit"
          class="feed__route feed__route--set"
          :style="{ background: bus.color || 'var(--color-accent)' }"
          :title="t('mixer.feeding.explicit')"
        ></span>
        <span
          v-else
          class="material-symbols-rounded feed__route feed__route--inherit"
          :title="row.via ? t('mixer.feeding.inheritedFrom', { name: row.via }) : t('mixer.feeding.inheritedDefault')"
        >subdirectory_arrow_right</span>
        <span class="feed__time">{{ itemTime(row.item) }}</span>
      </div>
    </template>
  </div>
</template>

<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from 'vue';
import type { AudioItem, Bus, GroupItem } from '~/types/project';
import { runTime, trimmedLength, formatRunTime } from '~/utils/groupTiming';
import { drawRowWaveform } from '~/utils/rowWaveform';

const props = defineProps<{
  bus: Bus;
}>();

const { t } = useLocalization();
const {
  currentProject,
  findItemByUuid,
  formatItemIndex,
  selectedItems,
  toggleItemSelection,
  waveformUpdateKey,
} = useProject();
const { activeCues, activeGroups } = useAudioEngine();

type Item = AudioItem | GroupItem;

interface SectionRow { kind: 'section'; key: string }
interface GroupRow {
  kind: 'group';
  key: string;
  group: GroupItem;
  depth: number;
  /** Audio cues inside the group (any depth) that feed this bus… */
  feeding: number;
  /** …out of all the audio cues inside it. */
  total: number;
}
interface AudioRow {
  kind: 'audio';
  key: string;
  item: AudioItem;
  depth: number;
  /** The cue names this bus itself, rather than inheriting it. */
  explicit: boolean;
  /** When inherited: the nearest group that sets a bus, if any. */
  via: string | null;
  /** Not in the playlist: a cart cue, with its slot (0-based) when known. */
  cart: boolean;
  slot: number | null;
}
type Row = SectionRow | GroupRow | AudioRow;

// ---- The tree -----------------------------------------------------------

function countAudio(items: Item[]): number {
  let n = 0;
  for (const it of items) n += it.type === 'group' ? countAudio((it as GroupItem).children ?? []) : 1;
  return n;
}

const rows = computed<Row[]>(() => {
  const wanted = new Set(props.bus.itemUuids ?? []);
  const out: Row[] = [];
  if (!wanted.size) return out;
  const placed = new Set<string>();

  // Returns how many wanted cues this level holds, emitting rows as it goes.
  // A group row is spliced in ahead of its children only once we know at
  // least one of them made it, so empty branches never show.
  const walk = (level: Item[], depth: number, chain: GroupItem[]): number => {
    let found = 0;
    for (const it of level) {
      if (it.type === 'group') {
        const g = it as GroupItem;
        const at = out.length;
        const inside = walk(g.children ?? [], depth + 1, [...chain, g]);
        if (inside) {
          out.splice(at, 0, {
            kind: 'group', key: 'g:' + g.uuid, group: g, depth,
            feeding: inside, total: countAudio(g.children ?? []),
          });
          found += inside;
        }
      } else if (wanted.has(it.uuid)) {
        const a = it as AudioItem;
        const explicit = a.busId === props.bus.id;
        let via: string | null = null;
        if (!explicit) {
          for (let i = chain.length - 1; i >= 0; i--) {
            if (chain[i]!.busId) { via = chain[i]!.displayName; break; }
          }
        }
        out.push({ kind: 'audio', key: 'a:' + a.uuid, item: a, depth, explicit, via, cart: false, slot: null });
        placed.add(a.uuid);
        found++;
      }
    }
    return found;
  };
  walk(currentProject.value?.items ?? [], 0, []);

  // Whatever the playlist does not hold is a cart cue, ordered by slot.
  const rest = [...wanted].filter(u => !placed.has(u));
  if (rest.length) {
    const slots = new Map<string, number>();
    for (const ci of currentProject.value?.cartItems ?? []) slots.set(ci.itemUuid, ci.slot);
    const cart: AudioRow[] = rest.map((uuid) => {
      const found = findItemByUuid(uuid);
      const item = (found && found.type === 'audio'
        ? found
        : { uuid, type: 'audio', displayName: '', color: '', index: [] }) as AudioItem;
      const slot = slots.get(uuid) ?? (item.index?.[0] === -1 ? item.index[1] ?? null : null);
      return {
        kind: 'audio', key: 'c:' + uuid, item, depth: 0,
        explicit: item.busId === props.bus.id, via: null,
        cart: true, slot,
      };
    });
    cart.sort((x, y) => (x.slot ?? Infinity) - (y.slot ?? Infinity));
    out.push({ kind: 'section', key: 'section:cart' }, ...cart);
  }
  return out;
});

// ---- Display --------------------------------------------------------------

// The playlist's own tint (PlaylistItem.vue): 25% of the item colour at rest,
// 50% while it plays. Tolerates a missing or short-form colour rather than
// painting NaNs.
function hexToRgba(hex: string | undefined, alpha: number): string {
  let h = (hex || '').replace('#', '');
  if (h.length === 3) h = h.split('').map(c => c + c).join('');
  if (!/^[0-9a-f]{6}/i.test(h)) return 'transparent';
  const r = parseInt(h.slice(0, 2), 16);
  const g = parseInt(h.slice(2, 4), 16);
  const b = parseInt(h.slice(4, 6), 16);
  return `rgba(${r}, ${g}, ${b}, ${alpha})`;
}

const INDENT = 10;
function rowStyle(color: string | undefined, depth: number, playing: boolean) {
  return {
    marginLeft: `${depth * INDENT}px`,
    backgroundColor: hexToRgba(color, playing ? 0.5 : 0.25),
  };
}

function progress(uuid: string): number {
  const c = activeCues.value.get(uuid);
  if (!c || c.duration <= 0) return 0;
  return Math.min(100, (c.currentTime / c.duration) * 100);
}

// Trimmed length at rest; a countdown while it plays, as on the playlist row.
function itemTime(item: AudioItem): string {
  const c = activeCues.value.get(item.uuid);
  if (c) return '-' + formatRunTime(Math.max(0, c.duration - c.currentTime));
  if (!item.duration) return '';
  return formatRunTime(trimmedLength(item));
}

function groupLive(uuid: string) {
  return activeGroups.value.get(uuid) ?? null;
}

// The group's whole run time, the figure its playlist row shows, so the two
// can be matched up at a glance.
function groupTime(row: GroupRow): string {
  const live = groupLive(row.group.uuid);
  if (live) return `-${formatRunTime(live.remaining)}${live.indefinite ? '+' : ''}`;
  const total = runTime(row.group);
  if (total.seconds <= 0 && !total.indefinite) return '';
  return `${formatRunTime(total.seconds)}${total.indefinite ? '+' : ''}`;
}
function groupTimeTitle(row: GroupRow): string {
  const total = runTime(row.group);
  const parts = [t('group.runTime', { time: formatRunTime(total.seconds) })];
  if (total.indefinite) parts.push(t('group.containsLoop'));
  return parts.join(' · ');
}

// A click selects the cue in the playlist, which reveals it (opening its
// group if need be) and scrolls it into view — PlaylistView watches the
// selection for exactly that. Cart cues have no playlist row to go to.
function select(uuid: string) {
  toggleItemSelection(uuid, false, false);
}

// ---- Waveforms ------------------------------------------------------------

const root = ref<HTMLElement | null>(null);
const canvases = new Map<string, HTMLCanvasElement>();
const uuidOf = new WeakMap<Element, string>();
const visible = new Set<string>();
let io: IntersectionObserver | null = null;
let ro: ResizeObserver | null = null;

function draw(uuid: string) {
  const canvas = canvases.get(uuid);
  const item = findItemByUuid(uuid);
  if (!canvas || !item || item.type !== 'audio') return;
  drawRowWaveform(canvas, item as AudioItem);
}
function drawVisible() {
  for (const uuid of visible) draw(uuid);
}

// One stable callback per uuid. An inline arrow would be a new function on
// every render, and Vue answers a changed function ref by unbinding and
// rebinding it — which here would mean unobserving and reobserving every
// canvas each time a playhead ticks.
const refFns = new Map<string, (el: unknown) => void>();
function canvasRef(uuid: string) {
  let fn = refFns.get(uuid);
  if (!fn) {
    fn = (el: unknown) => {
      const prev = canvases.get(uuid);
      if (el === prev) return;
      if (prev) { io?.unobserve(prev); canvases.delete(uuid); visible.delete(uuid); }
      if (el instanceof HTMLCanvasElement) {
        canvases.set(uuid, el);
        uuidOf.set(el, uuid);
        io?.observe(el);
      } else {
        refFns.delete(uuid);
      }
    };
    refFns.set(uuid, fn);
  }
  return fn;
}

onMounted(() => {
  io = new IntersectionObserver((entries) => {
    for (const e of entries) {
      const uuid = uuidOf.get(e.target);
      if (!uuid) continue;
      if (e.isIntersecting) {
        const was = visible.has(uuid);
        visible.add(uuid);
        if (!was) draw(uuid);
      } else {
        visible.delete(uuid);
      }
    }
  }, { rootMargin: '200px' });
  for (const el of canvases.values()) io.observe(el);

  // Every row is as wide as the list, so the list's own width is the only
  // size worth watching.
  let lastWidth = 0;
  ro = new ResizeObserver((entries) => {
    const w = entries[0]?.contentRect.width ?? 0;
    if (w === lastWidth) return;
    lastWidth = w;
    drawVisible();
  });
  if (root.value) ro.observe(root.value);
});

onBeforeUnmount(() => {
  io?.disconnect();
  ro?.disconnect();
  io = ro = null;
  canvases.clear();
  visible.clear();
});

// Redraw when anything that changes the picture changes: peaks arriving,
// a trim, a recolour, or the global "redraw waveforms" nudge.
watch(
  () => rows.value.map(r => r.kind === 'audio'
    ? [r.item.waveform?.peaks, r.item.inPoint, r.item.outPoint, r.item.color]
    : null),
  () => nextTick(drawVisible),
);
watch(waveformUpdateKey, () => nextTick(drawVisible));
</script>

<style scoped>
.feed {
  display: flex;
  flex-direction: column;
  gap: 2px;
  min-width: 0;
}

.feed__none { margin: 0; font-size: 11px; color: var(--color-text-disabled); }

/* The cart heading: a quiet divider, the same voice as the panel's own h4. */
.feed__section {
  display: flex;
  align-items: center;
  gap: 4px;
  margin: 6px 0 2px;
  font-size: 9px;
  letter-spacing: 0.08em;
  text-transform: uppercase;
  color: var(--color-text-secondary);
}
.feed__section .material-symbols-rounded { font-size: 12px; }

.feed__row {
  position: relative;
  display: flex;
  align-items: center;
  gap: 6px;
  height: 26px;
  padding: 0 6px 0 9px;
  min-width: 0;
  overflow: hidden;
  border-radius: var(--border-radius-sm);
  font-size: 11px;
  color: var(--color-text-primary);
  cursor: pointer;
  flex: 0 0 auto;
}
.feed__row.is-cart { cursor: default; }
.feed__row:hover:not(.is-cart) { box-shadow: inset 0 0 0 1px var(--color-border); }
.feed__row.is-selected { box-shadow: inset 0 0 0 1px var(--color-accent); }
.feed__row:focus-visible { outline: 1px solid var(--color-accent); outline-offset: -1px; }
/* Everything but the waveform and progress sits above them. */
.feed__row > :not(.feed__wave):not(.feed__progress):not(.feed__stripe) { position: relative; z-index: 2; }

.feed__stripe {
  position: absolute;
  z-index: 2;
  left: 0;
  top: 0;
  bottom: 0;
  width: 3px;
}

/* The playlist row's waveform, at the same strength (PlaylistItem.vue). */
.feed__wave {
  position: absolute;
  inset: 0;
  width: 100%;
  height: 100%;
  pointer-events: none;
  opacity: 0.1;
  z-index: 0;
}
.feed__row.is-playing .feed__wave { opacity: 0.2; }

/* A thin bar along the bottom rather than the playlist's full-height fill:
   at this size a full fill would hide the waveform it is meant to track. */
.feed__progress {
  position: absolute;
  left: 0;
  bottom: 0;
  height: 2px;
  z-index: 1;
  transition: width 100ms linear;
  pointer-events: none;
}

.feed__index {
  flex: 0 0 auto;
  min-width: 2.4em;
  font-family: var(--font-mono);
  font-size: 10px;
  color: var(--color-text-secondary);
}

.feed__folder {
  flex: 0 0 auto;
  font-size: 14px;
  color: var(--color-text-secondary);
}

.feed__name {
  flex: 1 1 auto;
  min-width: 0;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  font-weight: 600;
}
.feed__row--group .feed__name { font-weight: 700; }

.feed__time {
  flex: 0 0 auto;
  min-width: 3.4em;
  text-align: right;
  font-family: var(--font-mono);
  font-size: 10px;
  color: var(--color-text-secondary);
  white-space: nowrap;
}
.feed__row.is-playing .feed__time { color: var(--color-text-primary); }

/* Routing marks: small enough to ignore until you are asking the question. */
.feed__route { flex: 0 0 auto; cursor: help; }
.feed__route--set {
  width: 6px;
  height: 6px;
  border-radius: 50%;
}
.feed__route--inherit {
  font-size: 12px;
  color: var(--color-text-disabled);
}

.feed__partial {
  flex: 0 0 auto;
  padding: 0 4px;
  font-family: var(--font-mono);
  font-size: 9px;
  line-height: 13px;
  color: var(--color-text-secondary);
  background: var(--color-background);
  border-radius: 6px;
  cursor: help;
}
</style>

<template>
  <div class="item-events">
    <!-- The server owns events end to end; with no events route there is
         nothing here to edit, and empty forms would only suggest otherwise. -->
    <div v-if="pluginsStatus === 'unsupported'" class="ev-message">
      <span class="material-symbols-rounded">extension_off</span>
      <p>{{ t('events.unsupported') }}</p>
    </div>

    <div v-else-if="!(duration > 0)" class="ev-message">
      <span class="material-symbols-rounded">pending</span>
      <p>{{ t('properties.loadingAudioData') }}</p>
    </div>

    <template v-else>
      <div class="ev-toolbar">
        <Btn
          icon="add_location_alt"
          :text="t('events.addEvent')"
          :title="t('events.addEventHint')"
          :disabled="plugins.length === 0"
          @click="addEvent('time')"
        />
        <select
          class="ev-select ev-add-trigger"
          value=""
          :disabled="plugins.length === 0"
          :aria-label="t('events.addTriggerEvent')"
          @change="onAddTrigger"
        >
          <option value="" disabled>{{ t('events.addTriggerEvent') }}</option>
          <option v-for="trig in NON_TIME_TRIGGERS" :key="trig" :value="trig">{{ triggerLabel(trig) }}</option>
        </select>

        <label class="ev-cursor" :title="t('events.cursorHint')">
          <span class="material-symbols-rounded">vertical_align_center</span>
          <span>{{ t('events.cursor') }}</span>
          <input
            class="ev-time-input"
            type="text"
            :value="cursor === null ? '' : formatEventTime(cursor)"
            :placeholder="formatEventTime(inPoint)"
            @change="onCursorInput"
          />
        </label>

        <span class="ev-toolbar-spacer" />

        <span v-if="pluginsStatus === 'error' && plugins.length === 0" class="ev-toolbar-note">
          {{ t('events.loadFailed') }}
          <button type="button" class="ev-link-btn" @click="refreshPlugins()">{{ t('events.retry') }}</button>
        </span>
        <span v-else-if="pluginsStatus === 'unknown'" class="ev-toolbar-note">{{ t('events.loading') }}</span>
        <span v-else-if="plugins.length === 0" class="ev-toolbar-note">{{ t('events.noPlugins') }}</span>

        <Btn icon="cable" :text="t('events.manageConnections')" @click="openConnections" />
      </div>

      <!-- Timeline. The canvas only paints the waveform and the trim shading;
           markers, cursor and playhead are elements on top, so dragging one
           never repaints thousands of peaks. -->
      <div
        ref="timelineEl"
        class="ev-timeline"
        :class="{ panning: panning }"
        :style="markerPalette"
        @pointerdown="onTimelineDown"
        @pointermove="onTimelineMove"
        @pointerup="onTimelineUp"
        @pointercancel="onTimelineUp"
        @wheel.prevent="onWheel"
        @auxclick.prevent
      >
        <canvas ref="canvasEl" class="ev-canvas" />

        <!-- Where the cue starts and stops playing: the trim points. -->
        <div v-if="inView(inPoint) && inPoint > 0" class="ev-trim-line" :style="{ left: pct(inPoint) }" />
        <div v-if="inView(outPoint) && outPoint < duration" class="ev-trim-line" :style="{ left: pct(outPoint) }" />

        <div v-if="cursor !== null && inView(cursor)" class="ev-cursor-line" :style="{ left: pct(cursor) }" />

        <div v-if="playhead !== null && inView(playhead)" class="ev-playhead" :style="{ left: pct(playhead) }" />

        <!-- On start / On end events sit on the trim points they fire at.
             Not draggable: their time is the cue's, not theirs. -->
        <div
          v-for="m in edgeMarkers"
          v-show="inView(m.time)"
          :key="m.ev.id"
          class="ev-marker ev-marker--edge"
          :class="{
            selected: m.ev.id === selectedId,
            disabled: m.ev.enabled === false,
            flashing: isFlashing(m.ev.id),
            'ev-marker--end': m.end,
          }"
          :style="{ left: pct(m.time), '--stack': m.stack }"
          :title="markerTitle(m.ev)"
          @pointerdown.stop="onEdgeMarkerDown($event, m.ev)"
        >
          <span class="ev-marker-line" />
          <span class="ev-marker-flag">
            <span class="material-symbols-rounded ev-marker-icon">{{ m.end ? 'stop' : 'play_arrow' }}</span>
            {{ m.label }}
            <span v-if="lastResult[m.ev.id]?.ok === false" class="ev-error-dot" />
          </span>
        </div>

        <div
          v-for="m in markers"
          v-show="inView(m.time)"
          :key="m.ev.id"
          class="ev-marker"
          :class="{
            selected: m.ev.id === selectedId,
            disabled: m.ev.enabled === false,
            outside: m.outside,
            flashing: isFlashing(m.ev.id),
            dragging: drag?.id === m.ev.id,
          }"
          :style="{ left: pct(m.time), '--lane': m.lane }"
          :title="markerTitle(m.ev)"
          @pointerdown.stop="onMarkerDown($event, m.ev)"
          @pointermove="onMarkerMove"
          @pointerup="onMarkerUp"
          @pointercancel="onMarkerCancel"
        >
          <span class="ev-marker-line" />
          <span class="ev-marker-flag">
            {{ m.label }}
            <span v-if="lastResult[m.ev.id]?.ok === false" class="ev-error-dot" />
          </span>
        </div>
      </div>

      <!-- Navigation, as on the Playback tab: zoom, a scrollbar while zoomed,
           and Fit to see the whole file again. Wheel zooms at the pointer;
           Shift+wheel or a middle-button drag pans. -->
      <div class="ev-nav">
        <button type="button" class="ev-icon-btn" :title="t('events.zoomOut')" :disabled="zoom <= 1" @click="zoomBy(1 / 1.5)">
          <span class="material-symbols-rounded">zoom_out</span>
        </button>
        <input
          class="ev-zoom-slider"
          type="range"
          min="0"
          :max="ZOOM_STEPS"
          step="1"
          :value="zoomSlider"
          :aria-label="t('events.zoom')"
          @input="onZoomSlider"
        />
        <button type="button" class="ev-icon-btn" :title="t('events.zoomIn')" :disabled="zoom >= MAX_ZOOM" @click="zoomBy(1.5)">
          <span class="material-symbols-rounded">zoom_in</span>
        </button>
        <span class="ev-zoom-text">{{ Math.round(zoom * 100) }}%</span>
        <button type="button" class="ev-icon-btn" :title="t('events.zoomFit')" :disabled="zoom <= 1" @click="fitView">
          <span class="material-symbols-rounded">fit_screen</span>
        </button>
        <input
          v-if="zoom > 1"
          class="ev-scroll-slider"
          type="range"
          min="0"
          :max="maxViewStart"
          step="0.01"
          :value="viewStart"
          :aria-label="t('events.scroll')"
          @input="viewStart = Number(($event.target as HTMLInputElement).value)"
        />
        <span v-else class="ev-nav-hint">{{ t('events.navHint') }}</span>
      </div>

      <div class="ev-body">
        <!-- The list -->
        <div class="ev-list" role="listbox" :aria-label="t('events.tab')">
          <p v-if="sortedEvents.length === 0" class="ev-empty">{{ t('events.noEvents') }}</p>
          <div
            v-for="ev in sortedEvents"
            :key="ev.id"
            class="ev-row"
            role="option"
            :aria-selected="ev.id === selectedId"
            :class="{
              selected: ev.id === selectedId,
              disabled: ev.enabled === false,
              flashing: isFlashing(ev.id),
            }"
            @click="selectedId = ev.id"
          >
            <span class="ev-when" :class="{ 'ev-when--chip': ev.trigger !== 'time' }">
              {{ ev.trigger === 'time' ? formatEventTime(ev.time) : triggerLabel(ev.trigger) }}
            </span>
            <input
              type="checkbox"
              class="ev-row-toggle"
              :checked="ev.enabled !== false"
              :title="t('events.enabled')"
              @click.stop
              @change="setEnabled(ev, ($event.target as HTMLInputElement).checked)"
            />
            <span class="material-symbols-rounded ev-plugin-icon" :title="pluginOf(ev)?.label ?? ev.plugin">
              {{ pluginOf(ev)?.icon || 'extension' }}
            </span>
            <span class="ev-row-text">
              <span class="ev-row-title">{{ rowTitle(ev) }}</span>
              <span v-if="rowSubtitle(ev)" class="ev-row-sub">{{ rowSubtitle(ev) }}</span>
            </span>
            <span v-if="connectionLabel(ev)" class="ev-row-conn">{{ connectionLabel(ev) }}</span>
            <span
              class="ev-status"
              :class="statusClass(ev)"
              :title="statusTitle(ev)"
            />
            <span class="ev-row-actions">
              <button type="button" class="ev-icon-btn" :title="t('events.test')"
                      :disabled="testing.has(ev.id)" @click.stop="runTest(ev)">
                <span class="material-symbols-rounded" :class="{ spinning: testing.has(ev.id) }">
                  {{ testing.has(ev.id) ? 'progress_activity' : 'send' }}
                </span>
              </button>
              <button type="button" class="ev-icon-btn" :title="t('events.duplicate')" @click.stop="duplicateEvent(ev)">
                <span class="material-symbols-rounded">content_copy</span>
              </button>
              <button type="button" class="ev-icon-btn" :title="t('events.delete')" @click.stop="deleteEvent(ev)">
                <span class="material-symbols-rounded">delete</span>
              </button>
            </span>
          </div>
        </div>

        <!-- The editor for the selected event -->
        <div class="ev-editor">
          <p v-if="!selected" class="ev-empty">{{ t('events.selectEvent') }}</p>
          <template v-else>
            <div class="ev-editor-row">
              <div class="ev-field ev-field--grow">
                <label>{{ t('events.name') }}</label>
                <input
                  type="text"
                  :value="selected.name ?? ''"
                  :placeholder="rowTitle({ ...selected, name: '' })"
                  @change="setName(selected, ($event.target as HTMLInputElement).value)"
                />
              </div>
              <div class="ev-field">
                <label>{{ t('events.trigger.label') }}</label>
                <select :value="selected.trigger" @change="setTrigger(selected, ($event.target as HTMLSelectElement).value as EventTrigger)">
                  <option v-for="trig in EVENT_TRIGGERS" :key="trig" :value="trig">{{ triggerLabel(trig) }}</option>
                </select>
              </div>
              <div v-if="selected.trigger === 'time'" class="ev-field">
                <label>{{ t('events.time') }}</label>
                <div class="ev-input-with-btn">
                  <input
                    class="ev-time-input"
                    type="text"
                    :value="formatEventTime(selected.time)"
                    :title="t('events.timeHint')"
                    @change="onTimeInput(selected, $event)"
                  />
                  <button
                    type="button"
                    class="ev-icon-btn ev-icon-btn--boxed"
                    :disabled="cursor === null"
                    :title="t('events.useCursor')"
                    @click="setTime(selected, cursor!)"
                  >
                    <span class="material-symbols-rounded">vertical_align_center</span>
                  </button>
                </div>
              </div>
              <label class="ev-check">
                <input
                  type="checkbox"
                  :checked="selected.enabled !== false"
                  @change="setEnabled(selected, ($event.target as HTMLInputElement).checked)"
                />
                {{ t('events.enabled') }}
              </label>
            </div>

            <div class="ev-editor-row">
              <div class="ev-field">
                <label>{{ t('events.plugin') }}</label>
                <select :value="selected.plugin" @change="setPlugin(selected, ($event.target as HTMLSelectElement).value)">
                  <option v-for="p in plugins" :key="p.id" :value="p.id">{{ p.label }}</option>
                  <option v-if="!selectedPlugin" :value="selected.plugin">{{ selected.plugin }}</option>
                </select>
              </div>
              <div v-if="selectedPlugin?.needsConnection" class="ev-field ev-field--grow">
                <label>{{ t('events.connection') }}</label>
                <div class="ev-input-with-btn">
                  <select
                    :value="selected.connectionId ?? ''"
                    :class="{ invalid: !selectedConnection }"
                    @change="setConnection(selected, ($event.target as HTMLSelectElement).value)"
                  >
                    <option value="">{{ t('events.noConnection') }}</option>
                    <option v-for="c in selectedPluginConnections" :key="c.id" :value="c.id">
                      {{ c.name }}{{ c.enabled === false ? ` (${t('events.connections.disabled')})` : '' }}
                    </option>
                    <option v-if="selected.connectionId && !selectedConnection" :value="selected.connectionId">
                      {{ t('events.connectionMissing') }}
                    </option>
                  </select>
                  <button type="button" class="ev-icon-btn ev-icon-btn--boxed" :title="t('events.manageConnections')" @click="openConnections">
                    <span class="material-symbols-rounded">cable</span>
                  </button>
                </div>
              </div>
              <div v-if="selectedPlugin" class="ev-field ev-field--grow">
                <label>{{ t('events.action') }}</label>
                <select :value="selected.action" @change="setAction(selected, ($event.target as HTMLSelectElement).value)">
                  <option v-for="a in eventActions(selectedPlugin)" :key="a.id" :value="a.id">{{ a.label }}</option>
                  <option v-if="!selectedAction" :value="selected.action">{{ selected.action }}</option>
                </select>
              </div>
            </div>

            <p v-if="!selectedPlugin && pluginsStatus === 'ok'" class="ev-help ev-help--warn">
              {{ t('events.unknownPlugin', { plugin: selected.plugin }) }}
            </p>
            <p v-else-if="selectedPlugin?.needsConnection && selectedPluginConnections.length === 0" class="ev-help ev-help--warn">
              {{ t('events.noConnectionsForPlugin', { plugin: selectedPlugin.label }) }}
              <button type="button" class="ev-link-btn" @click="openConnections">{{ t('events.manageConnections') }}</button>
            </p>
            <p v-else-if="selectedConnection?.enabled === false" class="ev-help ev-help--warn">
              {{ t('events.connectionDisabled') }}
            </p>
            <p v-if="selected.trigger === 'time' && isOutside(selected.time)" class="ev-help ev-help--warn">
              {{ t('events.outsideTrim') }}
            </p>

            <SchemaForm
              v-if="selectedAction && selectedAction.fields.length > 0"
              :fields="selectedAction.fields"
              :model-value="selectedOptions"
              :options-context="{ plugin: selected!.plugin, connectionId: selected!.connectionId ?? null }"
              @update:model-value="(v: Record<string, unknown>) => setOptions(selected!, v)"
            />
            <p v-if="selectedAction && hasTextFields(selectedAction.fields)" class="ev-help">
              {{ t('events.placeholdersHelp') }}
            </p>

            <div class="ev-editor-footer">
              <Btn
                :icon="testing.has(selected.id) ? 'progress_activity' : 'send'"
                :text="testing.has(selected.id) ? t('events.testing') : t('events.test')"
                :disabled="testing.has(selected.id) || !selectedPlugin"
                @click="runTest(selected)"
              />
              <span v-if="testResults[selected.id]" class="ev-result" :class="testResults[selected.id]!.ok ? 'ok' : 'error'">
                {{ testResults[selected.id]!.ok ? t('events.testOk') : t('events.testFailed', { error: testResults[selected.id]!.error || '?' }) }}
              </span>
              <span v-else-if="lastResult[selected.id]" class="ev-result" :class="lastResult[selected.id]!.ok ? 'ok' : 'error'">
                {{ statusTitle(selected) }}
              </span>
            </div>
          </template>
        </div>
      </div>
    </template>
  </div>
</template>

<script setup lang="ts">
// The Events tab (#84): what this cue does to the outside world, placed on
// its own waveform. Click the waveform to put the cursor down, Add event drops
// one there; drag a marker to retime it. Everything is saved on the item like
// any other field — the parent's save path diffs it to the server — so every
// connected client sees the same events.
import { v4 as uuidv4 } from 'uuid';
import type { AudioItem } from '~/types/project';
import type { EventTrigger, FieldSchema, ItemEvent, PluginSchema } from '~/types/events';
import { EVENT_TRIGGERS } from '~/types/events';
import {
  connectionsFor,
  defaultsFor,
  eventActions,
  findAction,
  findPlugin,
  formatEventTime,
  formatSummary,
  makeEvent,
  parseEventTime,
  sortEvents,
  fillDefaults,
} from '~/utils/events';
import Btn from './Btn.vue';
import SchemaForm from './SchemaForm.vue';

const props = defineProps<{ item: AudioItem }>();
// The parent's save path (handleSave in PropertiesPanel).
const emit = defineEmits<{ change: [] }>();

const { t } = useLocalization();
const {
  plugins, pluginsStatus, connections, lastResult, isFlashing,
  refreshPlugins, testEvent,
} = useEvents();
const { activeCues } = useAudioEngine();

const NON_TIME_TRIGGERS: EventTrigger[] = ['start', 'pause', 'resume', 'end'];

const triggerLabel = (trig: EventTrigger) => t(`events.trigger.${trig}`);

// ---- the item's timeline --------------------------------------------------
const duration = computed(() => props.item.duration || props.item.waveform?.duration || 0);
const inPoint = computed(() => props.item.inPoint || 0);
const outPoint = computed(() => props.item.outPoint || duration.value);

const clampTime = (s: number) => Math.min(Math.max(0, s), duration.value);
// Millisecond resolution is what the editor shows; finer only adds noise to
// the saved file and to every diff.
const roundTime = (s: number) => Math.round(s * 1000) / 1000;
// ---- the view window -----------------------------------------------------
// The timeline shows [viewStart, viewStart + visibleDuration] of the file. At
// zoom 1 that's all of it; zoomed in, the wheel, the scrollbar or a middle-
// button drag moves the window. Every time<->pixel conversion goes through it.
const ZOOM_STEPS = 100;
const zoom = ref(1);
const viewStart = ref(0);
// Down to a quarter of a second across the timeline, and no further.
const MAX_ZOOM = computed(() => Math.max(1, Math.min(500, duration.value / 0.25)));
const visibleDuration = computed(() => (duration.value > 0 ? duration.value / zoom.value : 0));
const maxViewStart = computed(() => Math.max(0, duration.value - visibleDuration.value));
const viewEnd = computed(() => viewStart.value + visibleDuration.value);
const inView = (s: number) => s >= viewStart.value - 1e-6 && s <= viewEnd.value + 1e-6;
const pct = (s: number) =>
  `${visibleDuration.value > 0 ? ((s - viewStart.value) / visibleDuration.value) * 100 : 0}%`;

function setView(nextZoom: number, nextStart: number) {
  zoom.value = Math.min(MAX_ZOOM.value, Math.max(1, nextZoom));
  const vis = duration.value / zoom.value;
  viewStart.value = Math.min(Math.max(0, nextStart), Math.max(0, duration.value - vis));
}
// Zoom keeping `anchor` (a file time) where it is on screen.
function zoomAround(factor: number, anchor: number) {
  if (!(duration.value > 0)) return;
  const frac = visibleDuration.value > 0 ? (anchor - viewStart.value) / visibleDuration.value : 0.5;
  const nextZoom = Math.min(MAX_ZOOM.value, Math.max(1, zoom.value * factor));
  setView(nextZoom, anchor - frac * (duration.value / nextZoom));
}
// The buttons zoom around the cursor when it's on screen, else the middle.
function zoomBy(factor: number) {
  const anchor = cursor.value !== null && inView(cursor.value)
    ? cursor.value : viewStart.value + visibleDuration.value / 2;
  zoomAround(factor, anchor);
}
function fitView() { setView(1, 0); }
const zoomSlider = computed(() =>
  MAX_ZOOM.value > 1 ? Math.round((Math.log(zoom.value) / Math.log(MAX_ZOOM.value)) * ZOOM_STEPS) : 0);
function onZoomSlider(e: Event) {
  const v = Number((e.target as HTMLInputElement).value);
  zoomBy(Math.pow(MAX_ZOOM.value, v / ZOOM_STEPS) / zoom.value);
}
watch([duration, MAX_ZOOM], () => setView(zoom.value, viewStart.value));
// Another item: start from the whole file.
watch(() => props.item.uuid, fitView);
const isOutside = (s: number | undefined) =>
  typeof s === 'number' && (s < inPoint.value - 0.0005 || s > outPoint.value + 0.0005);

// Same source as the trimmer's playhead: the server's absolute file position
// when it sends one, so it lines up with times on this same file timeline.
const playhead = computed<number | null>(() => {
  const cue: any = activeCues.value.get(props.item.uuid);
  if (!cue) return null;
  if (typeof cue.playheadSeconds === 'number') return cue.playheadSeconds;
  return (cue.currentTime ?? 0) + inPoint.value;
});

// ---- events ---------------------------------------------------------------
const events = computed<ItemEvent[]>(() => props.item.events ?? []);
const sortedEvents = computed(() => sortEvents(events.value));

const selectedId = ref<string | null>(null);
const cursor = ref<number | null>(null);
// A marker being dragged, and where it is on screen until it is dropped.
const drag = ref<{ id: string; time: number; startX: number; moved: boolean } | null>(null);

const selected = computed(() => events.value.find(e => e.id === selectedId.value) ?? null);
const pluginOf = (ev: ItemEvent) => findPlugin(plugins.value, ev.plugin);
const actionOf = (ev: ItemEvent) => findAction(pluginOf(ev), ev.action);
const connectionOf = (ev: ItemEvent) =>
  ev.connectionId ? connections.value.find(c => c.id === ev.connectionId) ?? null : null;

const selectedPlugin = computed(() => (selected.value ? pluginOf(selected.value) : null));
const selectedAction = computed(() => (selected.value ? actionOf(selected.value) : null));
const selectedConnection = computed(() => (selected.value ? connectionOf(selected.value) : null));
const selectedPluginConnections = computed(() =>
  selectedPlugin.value ? connectionsFor(connections.value, selectedPlugin.value.id) : []);
// Shown with the schema's defaults filled in, so an unset field reads as what
// the server will use rather than as blank.
const selectedOptions = computed(() =>
  fillDefaults(selectedAction.value?.fields, selected.value?.options));

// A different cue is a different timeline: neither the cursor nor the
// selection means anything on it.
watch(() => props.item?.uuid, () => {
  selectedId.value = null;
  cursor.value = null;
  drag.value = null;
});

onMounted(() => { void refreshPlugins(); });

// ---- fades, as the server plays them ------------------------------------
// Mirrors play_item: a loop arms no crossfade, and Start Next supersedes it.
const looping = computed(() => props.item.endBehavior?.action === 'loop');
const startNextOn = computed(() =>
  !looping.value && !!props.item.startNextEnabled && (props.item.startNextTime ?? 0) > 0);
const crossFadeSec = computed(() =>
  looping.value || startNextOn.value ? 0 : Math.max(0, props.item.crossFade || 0));
// "On end" fires when the cue hands over: at the start of its crossfade, not
// at the out point (the sequencer retires it there).
const endFireTime = computed(() =>
  crossFadeSec.value > 0 ? Math.max(inPoint.value, outPoint.value - crossFadeSec.value) : outPoint.value);

// On start / On end events, drawn where they fire; several on one edge
// stack upwards.
const edgeMarkers = computed(() => {
  const out: { ev: ItemEvent; time: number; end: boolean; stack: number; label: string }[] = [];
  let starts = 0, ends = 0;
  for (const ev of sortedEvents.value) {
    if (ev.trigger !== 'start' && ev.trigger !== 'end') continue;
    const end = ev.trigger === 'end';
    out.push({ ev, end, time: end ? endFireTime.value : inPoint.value,
               stack: end ? ends++ : starts++, label: ev.name || rowTitle(ev) });
  }
  return out;
});

// ---- colour ---------------------------------------------------------------
// Markers take the cue's own colour: a darker shade normally, a lighter one
// with an outline when selected, so the selected flag is the one that stands
// out (fixed theme colours made an unselected red flag read as "selected").
// The waveform is drawn desaturated so neither competes with it.
type Rgb = [number, number, number];
const parseHex = (hex: string): Rgb | null => {
  const m = /^#?([0-9a-f]{6})$/i.exec((hex || '').trim());
  if (!m) return null;
  const n = parseInt(m[1]!, 16);
  return [(n >> 16) & 255, (n >> 8) & 255, n & 255];
};
const mix = (c: Rgb, to: number, amount: number) =>
  c.map(v => Math.round(v + (to - v) * amount)) as Rgb;
const rgbCss = (c: Rgb) => `rgb(${c[0]}, ${c[1]}, ${c[2]})`;
// Readable text on a flag: dark on light colours, light on dark ones.
const inkFor = (c: Rgb) =>
  (0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]) / 255 > 0.55 ? '#161616' : '#ffffff';
const trackRgb = computed<Rgb>(() => parseHex(props.item.color) ?? [255, 176, 32]);
const markerPalette = computed(() => {
  const dark = mix(trackRgb.value, 0, 0.3);
  const light = mix(trackRgb.value, 255, 0.55);
  return {
    '--marker-dark': rgbCss(dark), '--marker-dark-ink': inkFor(dark),
    '--marker-light': rgbCss(light), '--marker-light-ink': inkFor(light),
  } as Record<string, string>;
});
const waveformColour = computed(() => {
  const c = trackRgb.value;
  const grey = Math.round(0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2]);
  return rgbCss(mix(c, grey, 0.5));
});

// Flags that would overlap drop to the next lane down instead of covering
// each other: packed left to right by their measured width at this zoom.
// Past MAX_LANES they share the emptiest lane (hovering one lifts it).
const timelineWidth = ref(0);
const MAX_LANES = 3;
let measureCtx: CanvasRenderingContext2D | null = null;
function flagWidth(label: string, hasError: boolean): number {
  if (!measureCtx) {
    measureCtx = document.createElement('canvas').getContext('2d');
    if (measureCtx) measureCtx.font = `600 10px ${getComputedStyle(document.body).fontFamily}`;
  }
  const text = measureCtx ? measureCtx.measureText(label).width : label.length * 6;
  // Padding and border, the error dot, and the max-width the CSS applies.
  return Math.min(120, text + 12 + (hasError ? 9 : 0));
}

// Time markers, numbered in time order so the flag and the list agree.
const markers = computed(() => {
  const timed = sortedEvents.value.filter(e => e.trigger === 'time' && typeof e.time === 'number');
  const list = timed.map((ev, i) => {
    const time = drag.value?.id === ev.id ? drag.value.time : ev.time!;
    const label = drag.value?.id === ev.id ? formatEventTime(time) : (ev.name || String(i + 1));
    return { ev, time, label, outside: isOutside(time), lane: 0 };
  });
  const w = timelineWidth.value;
  if (w > 0 && visibleDuration.value > 0) {
    const laneEnds: number[] = [];
    for (const m of [...list].sort((a, b) => a.time - b.time)) {
      const x = ((m.time - viewStart.value) / visibleDuration.value) * w + 5;   // flag's left edge
      const right = x + flagWidth(m.label, lastResult.value[m.ev.id]?.ok === false);
      let lane = laneEnds.findIndex(end => end <= x - 2);
      if (lane < 0 && laneEnds.length < MAX_LANES) lane = laneEnds.length;
      if (lane < 0) lane = laneEnds.indexOf(Math.min(...laneEnds));
      laneEnds[lane] = Math.max(laneEnds[lane] ?? 0, right);
      m.lane = lane;
    }
  }
  return list;
});

const rowTitle = (ev: ItemEvent) => {
  if (ev.name) return ev.name;
  const action = actionOf(ev);
  if (action) return action.label;
  return `${ev.plugin} · ${ev.action}`;
};
const rowSubtitle = (ev: ItemEvent) => formatSummary(actionOf(ev), ev.options ?? {});

const connectionLabel = (ev: ItemEvent) => {
  if (!pluginOf(ev)?.needsConnection) return '';
  const c = connectionOf(ev);
  if (c) return c.name;
  return ev.connectionId ? t('events.connectionMissing') : t('events.noConnection');
};

const markerTitle = (ev: ItemEvent) => {
  const parts = [`${formatEventTime(ev.time)}  ${rowTitle(ev)}`];
  const sub = rowSubtitle(ev);
  if (sub) parts.push(sub);
  if (isOutside(ev.time)) parts.push(t('events.outsideTrim'));
  const r = lastResult.value[ev.id];
  if (r && !r.ok) parts.push(statusTitle(ev));
  return parts.join('\n');
};

const statusClass = (ev: ItemEvent) => {
  const r = lastResult.value[ev.id];
  if (!r) return 'none';
  return r.ok ? 'ok' : 'error';
};
const statusTitle = (ev: ItemEvent) => {
  const r = lastResult.value[ev.id];
  if (!r) return '';
  const at = new Date(r.at).toLocaleTimeString();
  return r.ok
    ? t('events.lastOk', { time: at })
    : t('events.lastError', { time: at, error: r.error || '?' });
};

const hasTextFields = (fields: FieldSchema[]): boolean =>
  fields.some(f => f.type === 'string' || f.type === 'text' || (f.type === 'list' && hasTextFields(f.fields ?? [])));

// ---- editing ----------------------------------------------------------------
// Edits go straight onto the item (it is the project's own reactive object,
// as everywhere else in the Properties panel) and then through the parent's
// save, which diffs the item to the server.
const commit = () => emit('change');

function ensureEvents(): ItemEvent[] {
  if (!Array.isArray(props.item.events)) props.item.events = [];
  return props.item.events;
}

// What a new event starts as. Cues for one desk tend to come in runs, so the
// selected event's plugin, connection and action are the best guess; failing
// that, the project's first enabled connection (an event with nowhere to go is
// no use), then a plugin that needs no connection.
function defaultsForNewEvent(): { plugin: PluginSchema; connectionId?: string | null; action?: string } | null {
  const sel = selected.value;
  const fromSel = sel && plugins.value.find(p => p.id === sel.plugin);
  if (sel && fromSel) return { plugin: fromSel, connectionId: sel.connectionId, action: sel.action };
  for (const c of connections.value) {
    if (c.enabled === false) continue;
    const p = plugins.value.find(pl => pl.id === c.plugin);
    if (p) return { plugin: p, connectionId: c.id };
  }
  const plugin = plugins.value.find(p => !p.needsConnection) ?? plugins.value[0];
  return plugin ? { plugin } : null;
}

function addEvent(trigger: EventTrigger) {
  const d = defaultsForNewEvent();
  if (!d) return;
  // At the cursor, else where the cue actually starts playing: an event at
  // 0 on a trimmed cue would sit in the shaded region and never fire.
  const time = trigger === 'time' ? roundTime(clampTime(cursor.value ?? inPoint.value)) : undefined;
  const ev = makeEvent(trigger, d.plugin, connections.value, time);
  if (d.connectionId !== undefined) ev.connectionId = d.connectionId;
  const action = d.action ? eventActions(d.plugin).find(a => a.id === d.action) : undefined;
  if (action) {
    ev.action = action.id;
    ev.options = defaultsFor(action.fields);
  }
  ensureEvents().push(ev);
  selectedId.value = ev.id;
  commit();
}

function onAddTrigger(e: Event) {
  const sel = e.target as HTMLSelectElement;
  const trig = sel.value as EventTrigger;
  sel.value = '';
  if (trig) addEvent(trig);
}

function duplicateEvent(ev: ItemEvent) {
  const list = ensureEvents();
  const copy: ItemEvent = { ...JSON.parse(JSON.stringify(ev)), id: uuidv4() };
  const i = list.findIndex(e => e.id === ev.id);
  list.splice(i < 0 ? list.length : i + 1, 0, copy);
  selectedId.value = copy.id;
  commit();
}

function deleteEvent(ev: ItemEvent) {
  const list = ensureEvents();
  const i = list.findIndex(e => e.id === ev.id);
  if (i < 0) return;
  if (selectedId.value === ev.id) {
    // Keep the selection in the list where it was, so deleting a run of
    // events is a run of clicks on the same spot.
    const order = sortedEvents.value;
    const at = order.findIndex(e => e.id === ev.id);
    selectedId.value = (order[at + 1] ?? order[at - 1])?.id ?? null;
  }
  list.splice(i, 1);
  commit();
}

function setEnabled(ev: ItemEvent, on: boolean) {
  ev.enabled = on;
  commit();
}

function setName(ev: ItemEvent, name: string) {
  const v = name.trim();
  if (v) ev.name = v;
  else delete ev.name;
  commit();
}

function setTime(ev: ItemEvent, s: number) {
  ev.time = roundTime(clampTime(s));
  commit();
}

function onTimeInput(ev: ItemEvent, e: Event) {
  const input = e.target as HTMLInputElement;
  const parsed = parseEventTime(input.value);
  if (parsed === null) { input.value = formatEventTime(ev.time); return; }
  setTime(ev, parsed);
  input.value = formatEventTime(ev.time);
}

function onCursorInput(e: Event) {
  const input = e.target as HTMLInputElement;
  if (input.value.trim() === '') { cursor.value = null; return; }
  const parsed = parseEventTime(input.value);
  if (parsed === null) {
    input.value = cursor.value === null ? '' : formatEventTime(cursor.value);
    return;
  }
  cursor.value = roundTime(clampTime(parsed));
  input.value = formatEventTime(cursor.value);
}

function setTrigger(ev: ItemEvent, trig: EventTrigger) {
  if (ev.trigger === trig) return;
  ev.trigger = trig;
  if (trig === 'time') {
    if (typeof ev.time !== 'number') ev.time = roundTime(clampTime(cursor.value ?? inPoint.value));
  } else {
    delete ev.time;
  }
  commit();
}

// A different plugin or action is a different form: its options start again
// from that schema's defaults rather than carrying fields it doesn't have.
function setPlugin(ev: ItemEvent, pluginId: string) {
  const plugin = findPlugin(plugins.value, pluginId);
  if (!plugin || ev.plugin === pluginId) return;
  const action = eventActions(plugin)[0];
  ev.plugin = plugin.id;
  ev.action = action?.id ?? '';
  ev.connectionId = plugin.needsConnection
    ? connectionsFor(connections.value, plugin.id)[0]?.id ?? null
    : null;
  ev.options = fillDefaults(action?.fields, {});
  commit();
}

function setAction(ev: ItemEvent, actionId: string) {
  if (ev.action === actionId) return;
  const action = findAction(pluginOf(ev), actionId);
  ev.action = actionId;
  ev.options = fillDefaults(action?.fields, {});
  commit();
}

function setConnection(ev: ItemEvent, id: string) {
  ev.connectionId = id || null;
  commit();
}

function setOptions(ev: ItemEvent, options: Record<string, unknown>) {
  ev.options = options;
  commit();
}

// ---- test -----------------------------------------------------------------
const testing = ref(new Set<string>());
const testResults = ref<Record<string, { ok: boolean; error?: string }>>({});

async function runTest(ev: ItemEvent) {
  if (testing.value.has(ev.id)) return;
  testing.value = new Set(testing.value).add(ev.id);
  const { [ev.id]: _old, ...rest } = testResults.value;
  testResults.value = rest;
  try {
    const res = await testEvent(
      { plugin: ev.plugin, action: ev.action, options: ev.options ?? {}, connectionId: ev.connectionId ?? null },
      { itemUuid: props.item.uuid },
    );
    testResults.value = { ...testResults.value, [ev.id]: res };
  } finally {
    const next = new Set(testing.value);
    next.delete(ev.id);
    testing.value = next;
  }
}

const openConnections = () => useSettingsPage().open('connections');

// ---- pointer: cursor and marker drag ---------------------------------------
const timelineEl = ref<HTMLDivElement | null>(null);
const canvasEl = ref<HTMLCanvasElement | null>(null);

const timeAt = (clientX: number): number => {
  const el = timelineEl.value;
  if (!el || !(duration.value > 0)) return 0;
  const r = el.getBoundingClientRect();
  const frac = r.width > 0 ? (clientX - r.left) / r.width : 0;
  return clampTime(viewStart.value + frac * visibleDuration.value);
};

// Wheel: zoom at the pointer. Shift+wheel, or a sideways wheel/trackpad: pan.
function onWheel(e: WheelEvent) {
  if (!(duration.value > 0)) return;
  const sideways = e.shiftKey || Math.abs(e.deltaX) > Math.abs(e.deltaY);
  if (sideways) {
    const w = timelineEl.value?.clientWidth || 1;
    const px = Math.abs(e.deltaX) > Math.abs(e.deltaY) ? e.deltaX : e.deltaY;
    setView(zoom.value, viewStart.value + (px / w) * visibleDuration.value);
    return;
  }
  zoomAround(e.deltaY < 0 ? 1.25 : 1 / 1.25, timeAt(e.clientX));
}

// Middle-button drag pans, wherever it starts.
const panning = ref(false);
let panFromX = 0;
let panFromStart = 0;

// Background: press puts the cursor down, and dragging scrubs it.
let scrubbing = false;
function onTimelineDown(e: PointerEvent) {
  if (e.button === 1) {
    e.preventDefault();   // no autoscroll cursor
    panning.value = true;
    panFromX = e.clientX;
    panFromStart = viewStart.value;
    timelineEl.value?.setPointerCapture?.(e.pointerId);
    return;
  }
  if (e.button !== 0) return;
  scrubbing = true;
  (e.currentTarget as HTMLElement).setPointerCapture?.(e.pointerId);
  cursor.value = roundTime(timeAt(e.clientX));
}
function onTimelineMove(e: PointerEvent) {
  if (panning.value) {
    const w = timelineEl.value?.clientWidth || 1;
    setView(zoom.value, panFromStart - ((e.clientX - panFromX) / w) * visibleDuration.value);
    return;
  }
  if (!scrubbing) return;
  cursor.value = roundTime(timeAt(e.clientX));
}
function onTimelineUp(e: PointerEvent) {
  if (panning.value) {
    panning.value = false;
    timelineEl.value?.releasePointerCapture?.(e.pointerId);
    return;
  }
  if (!scrubbing) return;
  scrubbing = false;
  (e.currentTarget as HTMLElement).releasePointerCapture?.(e.pointerId);
}

// Marker: press selects it; moving more than a few pixels turns it into a
// drag. The time on screen follows the pointer (`drag`, above), but the item
// is only written on release — one PATCH per retime, not one per pixel.
const DRAG_THRESHOLD_PX = 3;

// Start/end flags only select: they move with the trim points.
function onEdgeMarkerDown(e: PointerEvent, ev: ItemEvent) {
  if (e.button === 1) { onTimelineDown(e); return; }
  if (e.button === 0) selectedId.value = ev.id;
}

function onMarkerDown(e: PointerEvent, ev: ItemEvent) {
  if (e.button === 1) { onTimelineDown(e); return; }
  if (e.button !== 0) return;
  selectedId.value = ev.id;
  (e.currentTarget as HTMLElement).setPointerCapture?.(e.pointerId);
  drag.value = { id: ev.id, time: ev.time ?? 0, startX: e.clientX, moved: false };
}
function onMarkerMove(e: PointerEvent) {
  const d = drag.value;
  if (!d) return;
  if (!d.moved && Math.abs(e.clientX - d.startX) < DRAG_THRESHOLD_PX) return;
  d.moved = true;
  d.time = roundTime(timeAt(e.clientX));
}
function onMarkerUp(e: PointerEvent) {
  const d = drag.value;
  if (!d) return;
  (e.currentTarget as HTMLElement).releasePointerCapture?.(e.pointerId);
  drag.value = null;
  if (!d.moved) return;
  const ev = events.value.find(x => x.id === d.id);
  if (ev && ev.time !== d.time) setTime(ev, d.time);
}
function onMarkerCancel() {
  drag.value = null;
}

// ---- waveform paint -----------------------------------------------------------
// The whole file, not the trimmed region the playlist rows show: event times
// are file times, and an event in the trimmed-off part still has to be
// visible to be moved back in.
function draw() {
  const canvas = canvasEl.value;
  const box = timelineEl.value;
  if (!canvas || !box) return;
  const w = box.clientWidth;
  const h = box.clientHeight;
  if (w <= 0 || h <= 0) return;
  timelineWidth.value = w;
  const dpr = window.devicePixelRatio || 1;
  canvas.width = Math.round(w * dpr);
  canvas.height = Math.round(h * dpr);
  const ctx = canvas.getContext('2d');
  if (!ctx) return;
  ctx.scale(dpr, dpr);

  const css = getComputedStyle(document.documentElement);
  ctx.fillStyle = css.getPropertyValue('--color-background').trim() || '#161616';
  ctx.fillRect(0, 0, w, h);

  const dur = duration.value;
  const peaks = props.item.waveform?.peaks;
  const mid = h / 2;
  const v0 = viewStart.value;
  const vis = visibleDuration.value;
  const xOf = (t: number) => ((t - v0) / vis) * w;
  if (peaks && peaks.length > 0 && dur > 0 && vis > 0) {
    ctx.fillStyle = waveformColour.value;
    ctx.globalAlpha = 0.75;
    // One bar per CSS pixel, the loudest peak in its span: a long file has
    // far more peaks than pixels, and striding over them drops transients.
    const n = peaks.length;
    for (let x = 0; x < w; x++) {
      const a = Math.floor(((v0 + (x / w) * vis) / dur) * n);
      const b = Math.max(a + 1, Math.floor(((v0 + ((x + 1) / w) * vis) / dur) * n));
      let m = 0;
      for (let i = a; i < b && i < n; i++) if (peaks[i]! > m) m = peaks[i]!;
      // Gamma 2, as the row waveforms: dynamics without loud tracks filling the box.
      const v = Math.min(1, m);
      const bh = Math.max(1, v * v * h * 0.9);
      ctx.fillRect(x, mid - bh / 2, 1, bh);
    }
    ctx.globalAlpha = 1;
  } else {
    ctx.strokeStyle = css.getPropertyValue('--color-border').trim() || '#525252';
    ctx.beginPath();
    ctx.moveTo(0, mid);
    ctx.lineTo(w, mid);
    ctx.stroke();
  }

  // Shade what the trim cuts off: events there exist but never fire.
  if (dur > 0 && vis > 0) {
    ctx.fillStyle = 'rgba(0, 0, 0, 0.55)';
    const xi = Math.min(w, Math.max(0, xOf(inPoint.value)));
    const xo = Math.min(w, Math.max(0, xOf(outPoint.value)));
    if (xi > 0) ctx.fillRect(0, 0, xi, h);
    if (xo < w) ctx.fillRect(xo, 0, w - xo, h);
  }

  // Fades, in the Playback tab's colours: the play fade and stop fade red,
  // a crossfade yellow (the next cue's ramp dashed, crossing this one's),
  // Start Next a green line. Ramps run between the flag rows.
  if (dur > 0 && vis > 0) {
    const top = 56, bottom = h - 30;   // between the top flags and the edge flags
    const RED = '255, 77, 77', YELLOW = '255, 210, 63', GREEN = '74, 222, 128';
    const ramp = (t0: number, t1: number, up: boolean, rgb: string, label: string, dashedReverse = false) => {
      const x0 = xOf(t0), x1 = xOf(t1);
      if (x1 < 0 || x0 > w || x1 - x0 < 1) return;
      const y0 = up ? bottom : top, y1 = up ? top : bottom;
      ctx.fillStyle = `rgba(${rgb}, 0.12)`;
      ctx.beginPath();
      ctx.moveTo(x0, bottom); ctx.lineTo(x0, y0); ctx.lineTo(x1, y1); ctx.lineTo(x1, bottom);
      ctx.closePath();
      ctx.fill();
      ctx.strokeStyle = `rgba(${rgb}, 0.9)`;
      ctx.lineWidth = 1.5;
      ctx.beginPath(); ctx.moveTo(x0, y0); ctx.lineTo(x1, y1); ctx.stroke();
      if (dashedReverse) {
        ctx.setLineDash([4, 3]);
        ctx.beginPath(); ctx.moveTo(x0, y1); ctx.lineTo(x1, y0); ctx.stroke();
        ctx.setLineDash([]);
      }
      ctx.lineWidth = 1;
      ctx.font = '10px sans-serif';
      const tw = ctx.measureText(label).width;
      if (x1 - x0 > tw + 8) {
        ctx.fillStyle = `rgba(${rgb}, 1)`;
        ctx.fillText(label, Math.max(x0, 0) + 4, bottom - 4);
      }
    };
    const playFade = Math.max(0, props.item.playFade || 0);
    const stopFade = Math.max(0, props.item.stopFade || 0);
    const loopXf = looping.value ? Math.max(0, props.item.loopCrossfade || 0) : 0;
    if (playFade > 0)
      ramp(inPoint.value, Math.min(outPoint.value, inPoint.value + playFade), true, RED,
           t('waveform.playFadeTitle', { seconds: playFade.toFixed(1) }));
    if (stopFade > 0 && !looping.value)
      ramp(Math.max(inPoint.value, outPoint.value - stopFade), outPoint.value, false, RED,
           t('waveform.stopFadeTitle', { seconds: stopFade.toFixed(1) }));
    if (crossFadeSec.value > 0)
      ramp(endFireTime.value, outPoint.value, false, YELLOW,
           t('waveform.crossFadeTitle', { seconds: crossFadeSec.value.toFixed(1) }), true);
    if (loopXf > 0)
      ramp(Math.max(inPoint.value, outPoint.value - loopXf), outPoint.value, false, YELLOW,
           `${t('properties.loopCrossfade')}: ${loopXf.toFixed(1)}s`, true);
    if (startNextOn.value) {
      const xs = xOf(props.item.startNextTime!);
      if (xs >= 0 && xs <= w) {
        ctx.strokeStyle = `rgba(${GREEN}, 0.9)`;
        ctx.setLineDash([5, 3]);
        ctx.beginPath(); ctx.moveTo(xs, 0); ctx.lineTo(xs, h); ctx.stroke();
        ctx.setLineDash([]);
        ctx.fillStyle = `rgba(${GREEN}, 1)`;
        ctx.font = '10px sans-serif';
        ctx.fillText(t('waveform.startNextTitle', { time: formatEventTime(props.item.startNextTime!) }),
                     xs + 4, bottom - 4);
      }
    }
  }

  // Time ticks along the bottom, at whichever round step lands ~80px apart.
  if (dur > 0 && vis > 0) {
    const steps = [0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600];
    const want = (80 / w) * vis;
    const step = steps.find(s => s >= want) ?? steps[steps.length - 1]!;
    const decimals = step < 0.1 ? 2 : step < 1 ? 1 : 0;
    ctx.fillStyle = 'rgba(128, 128, 128, 0.8)';
    ctx.font = '10px sans-serif';
    for (let k = Math.ceil(v0 / step); k * step < Math.min(dur, v0 + vis); k++) {
      const s = k * step;
      if (s <= 0) continue;
      const x = xOf(s);
      ctx.fillRect(x, h - 4, 1, 4);
      const m = Math.floor(s / 60);
      const sec = s % 60;
      const label = decimals > 0
        ? `${m}:${sec.toFixed(decimals).padStart(3 + decimals, '0')}`
        : `${m}:${String(Math.floor(sec + 1e-6)).padStart(2, '0')}`;
      ctx.fillText(label, x + 3, h - 3);
    }
  }
}

let rafId = 0;
const scheduleDraw = () => {
  if (rafId) return;
  rafId = requestAnimationFrame(() => { rafId = 0; draw(); });
};

watch(
  () => [props.item.waveform, props.item.color, inPoint.value, outPoint.value, duration.value,
         zoom.value, viewStart.value, props.item.playFade, props.item.stopFade, props.item.crossFade,
         props.item.loopCrossfade, props.item.startNextEnabled, props.item.startNextTime,
         props.item.endBehavior?.action],
  scheduleDraw,
);

// Selecting an event in the list brings its marker into view.
watch(selectedId, (id) => {
  const ev = events.value.find(e => e.id === id);
  if (!ev || zoom.value <= 1) return;
  const at = ev.trigger === 'time' ? ev.time
    : ev.trigger === 'start' ? inPoint.value : ev.trigger === 'end' ? endFireTime.value : undefined;
  if (typeof at === 'number' && !inView(at)) setView(zoom.value, at - visibleDuration.value * 0.25);
});

// The timeline only exists once the duration is known; (re)attach the
// observer whenever the element itself is swapped in.
let resizeObserver: ResizeObserver | null = null;
watch(timelineEl, (el) => {
  resizeObserver?.disconnect();
  resizeObserver = null;
  if (!el) return;
  resizeObserver = new ResizeObserver(scheduleDraw);
  resizeObserver.observe(el);
  scheduleDraw();
}, { flush: 'post' });

onBeforeUnmount(() => {
  resizeObserver?.disconnect();
  if (rafId) cancelAnimationFrame(rafId);
});
</script>

<style scoped>
.item-events {
  display: flex;
  flex-direction: column;
  gap: var(--spacing-sm);
  flex: 1;
  min-height: 0;
  min-width: 640px;
  color: var(--color-text-secondary);
}

.ev-message {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: var(--spacing-sm);
  padding: var(--spacing-xl);
  text-align: center;

  .material-symbols-rounded {
    font-size: 36px;
  }

  p {
    font-size: 14px;
    max-width: 460px;
  }
}

/* ---- Toolbar ----------------------------------------------------------- */
.ev-toolbar {
  display: flex;
  align-items: center;
  gap: var(--spacing-sm);
  flex: 0 0 auto;
  flex-wrap: wrap;
}

.ev-toolbar-spacer {
  flex: 1;
}

.ev-toolbar-note {
  font-size: 12px;
}

.ev-select,
.ev-field input,
.ev-field select,
.ev-time-input {
  padding: var(--spacing-xs) var(--spacing-sm);
  min-height: 30px;
  background-color: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: 4px;
  color: var(--color-text-primary);
  font-size: 13px;

  &:focus {
    outline: none;
    border-color: var(--color-accent);
  }

  &.invalid {
    border-color: #e53e3e;
  }
}

.ev-add-trigger {
  min-height: 35px;
}

.ev-cursor {
  display: flex;
  align-items: center;
  gap: var(--spacing-xs);
  font-size: 12px;
  margin-left: var(--spacing-sm);

  .material-symbols-rounded {
    font-size: 16px;
  }
}

.ev-time-input {
  width: 96px;
  font-family: var(--font-mono);
  font-variant-numeric: tabular-nums;
}

.ev-link-btn {
  background: none;
  border: none;
  padding: 0;
  color: var(--color-accent);
  cursor: pointer;
  font-size: inherit;
  text-decoration: underline;
}

/* ---- Timeline ---------------------------------------------------------- */
.ev-timeline {
  position: relative;
  flex: 0 0 auto;
  height: 120px;
  border: 1px solid var(--color-border);
  border-radius: 4px;
  overflow: hidden;
  cursor: crosshair;
  touch-action: none;
  user-select: none;
}

.ev-canvas {
  position: absolute;
  inset: 0;
  width: 100%;
  height: 100%;
  display: block;
}

.ev-cursor-line,
.ev-playhead {
  position: absolute;
  top: 0;
  bottom: 0;
  width: 0;
  pointer-events: none;
}

.ev-cursor-line {
  border-left: 1px dashed var(--color-text-primary);
  opacity: 0.8;
}

.ev-playhead {
  border-left: 2px solid var(--color-text-primary);
  box-shadow: 0 0 4px rgba(0, 0, 0, 0.6);
}

.ev-timeline.panning { cursor: grabbing; }

/* Where the cue starts and stops: a quiet line on each trim point. */
.ev-trim-line {
  position: absolute;
  top: 0;
  bottom: 0;
  width: 0;
  border-left: 1px solid rgba(255, 255, 255, 0.35);
  pointer-events: none;
}

/* Marker colours come from the cue's colour (markerPalette): darker normally,
   lighter and outlined when selected. */
.ev-marker {
  --marker-color: var(--marker-dark, var(--color-warning));
  --marker-ink: var(--marker-dark-ink, #161616);
  position: absolute;
  top: 0;
  bottom: 0;
  width: 12px;
  margin-left: -6px;
  cursor: ew-resize;
  /* No stacking context of its own: every marker's line then sits under every
     marker's flag, so a neighbour's line never cuts through a label. */

  &.selected {
    --marker-color: var(--marker-light, var(--color-accent));
    --marker-ink: var(--marker-light-ink, #161616);
  }

  &.selected .ev-marker-flag { z-index: 3; }

  &.selected .ev-marker-flag {
    box-shadow: 0 0 0 1px #fff, 0 1px 4px rgba(0, 0, 0, 0.6);
  }

  &.selected .ev-marker-line {
    width: 3px;
    left: 4px;
    box-shadow: 0 0 3px rgba(255, 255, 255, 0.6);
  }

  /* Dimmed on the parts, not the marker: opacity on the marker would give it a
     stacking context and put its flag back under its neighbours' lines. */
  &.disabled .ev-marker-line,
  &.disabled .ev-marker-flag,
  &.outside .ev-marker-line,
  &.outside .ev-marker-flag {
    opacity: 0.45;
  }

  /* Flags sharing the last lane overlap; the one under the pointer comes up. */
  &:hover .ev-marker-flag { z-index: 4; }
  &.dragging .ev-marker-flag { z-index: 5; }

  &.flashing .ev-marker-line,
  &.flashing .ev-marker-flag {
    animation: ev-flash 450ms ease-out;
  }
}

.ev-marker-line {
  position: absolute;
  z-index: 1;
  top: 0;
  bottom: 0;
  left: 5px;
  width: 2px;
  background: var(--marker-color);
}

.ev-marker.disabled .ev-marker-line {
  background: repeating-linear-gradient(to bottom, var(--marker-color) 0 4px, transparent 4px 7px);
}

.ev-marker-flag {
  position: absolute;
  z-index: 2;
  top: calc(var(--lane, 0) * 17px);
  left: 5px;
  display: flex;
  align-items: center;
  gap: 3px;
  max-width: 120px;
  padding: 1px 5px;
  border-radius: 0 3px 3px 0;
  background: var(--marker-color);
  color: var(--marker-ink);
  border: 1px solid rgba(255, 255, 255, 0.25);
  font-size: 10px;
  font-weight: 600;
  line-height: 14px;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
  font-variant-numeric: tabular-nums;
}

/* On start / On end: flags along the bottom edge (time markers use the top),
   stacked upwards; an end flag opens to the left so it stays on screen. */
.ev-marker--edge {
  cursor: pointer;
}

.ev-marker--edge .ev-marker-flag {
  top: auto;
  bottom: calc(14px + var(--stack, 0) * 17px);
}

.ev-marker--end .ev-marker-flag {
  left: auto;
  right: 5px;
  border-radius: 3px 0 0 3px;
}

.ev-marker-icon {
  font-size: 11px;
  line-height: 1;
}

/* ---- Navigation under the timeline ------------------------------------- */
.ev-nav {
  display: flex;
  align-items: center;
  gap: 6px;
  min-height: 26px;
  margin-top: -2px;
}

.ev-zoom-slider {
  width: 140px;
}

.ev-zoom-text {
  min-width: 48px;
  font-size: 11px;
  color: var(--color-text-secondary);
  font-variant-numeric: tabular-nums;
}

.ev-scroll-slider {
  flex: 1;
  min-width: 80px;
}

.ev-nav-hint {
  flex: 1;
  font-size: 11px;
  color: var(--color-text-secondary);
  text-align: right;
}

.ev-error-dot {
  flex: 0 0 auto;
  width: 6px;
  height: 6px;
  border-radius: 50%;
  background: #e53e3e;
  box-shadow: 0 0 0 1px #fff;
}

/* ---- Body: list + editor ------------------------------------------------ */
.ev-body {
  display: flex;
  gap: var(--spacing-md);
  flex: 1;
  min-height: 140px;
}

.ev-list {
  flex: 0 0 42%;
  min-width: 300px;
  overflow-y: auto;
  border: 1px solid var(--color-border);
  border-radius: 4px;
  background: var(--color-background);
}

.ev-empty {
  padding: var(--spacing-md);
  font-size: 12px;
}

.ev-row {
  display: flex;
  align-items: center;
  gap: var(--spacing-sm);
  padding: 4px var(--spacing-sm);
  border-bottom: 1px solid var(--color-border);
  font-size: 12px;
  cursor: pointer;

  &:hover {
    background: var(--color-surface-hover);
  }

  &.selected {
    background: var(--color-surface-hover);
    box-shadow: inset 3px 0 0 var(--color-accent);
  }

  &.disabled .ev-row-text,
  &.disabled .ev-when,
  &.disabled .ev-plugin-icon {
    opacity: 0.5;
  }

  &.flashing {
    animation: ev-row-flash 450ms ease-out;
  }
}

.ev-when {
  flex: 0 0 74px;
  font-family: var(--font-mono);
  font-variant-numeric: tabular-nums;
  color: var(--color-text-primary);
}

.ev-when--chip {
  font-family: inherit;
  font-size: 11px;
  text-align: center;
  padding: 1px 6px;
  border: 1px solid var(--color-border);
  border-radius: 10px;
  color: var(--color-text-secondary);
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}

.ev-row-toggle {
  flex: 0 0 auto;
  width: 14px;
  height: 14px;
  accent-color: var(--color-accent);
  cursor: pointer;
}

.ev-plugin-icon {
  flex: 0 0 auto;
  font-size: 16px;
}

.ev-row-text {
  flex: 1;
  min-width: 0;
  display: flex;
  gap: var(--spacing-xs);
  align-items: baseline;
  overflow: hidden;
  white-space: nowrap;
}

.ev-row-title {
  color: var(--color-text-primary);
  overflow: hidden;
  text-overflow: ellipsis;
  flex-shrink: 0;
  max-width: 60%;
}

.ev-row-sub {
  overflow: hidden;
  text-overflow: ellipsis;
  font-family: var(--font-mono);
  font-size: 11px;
}

.ev-row-conn {
  flex: 0 1 auto;
  max-width: 110px;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  font-size: 11px;
}

.ev-status {
  flex: 0 0 auto;
  width: 8px;
  height: 8px;
  border-radius: 50%;

  &.ok { background: var(--color-success); }
  &.error { background: #e53e3e; }
  &.none { background: transparent; }
}

.ev-row-actions {
  display: flex;
  gap: 2px;
  flex: 0 0 auto;
}

.ev-icon-btn {
  display: flex;
  align-items: center;
  justify-content: center;
  width: 24px;
  height: 24px;
  background: none;
  border: 1px solid transparent;
  border-radius: 4px;
  color: var(--color-text-secondary);
  cursor: pointer;

  .material-symbols-rounded {
    font-size: 16px;
  }

  &:hover:not(:disabled) {
    color: var(--color-text-primary);
    background: var(--color-surface-hover);
  }

  &:disabled {
    opacity: 0.4;
    cursor: not-allowed;
  }

  .spinning {
    animation: ev-spin 1s linear infinite;
  }
}

.ev-icon-btn--boxed {
  width: 30px;
  height: 30px;
  flex: 0 0 auto;
  border-color: var(--color-border);
  background: var(--color-surface-hover);
}

/* ---- Editor ------------------------------------------------------------ */
.ev-editor {
  flex: 1;
  min-width: 0;
  overflow-y: auto;
  display: flex;
  flex-direction: column;
  gap: var(--spacing-sm);
  padding-right: var(--spacing-xs);
}

.ev-editor-row {
  display: flex;
  flex-wrap: wrap;
  gap: var(--spacing-sm) var(--spacing-md);
  align-items: flex-end;
}

.ev-field {
  display: flex;
  flex-direction: column;
  gap: var(--spacing-xs);
  min-width: 120px;

  label {
    font-size: 12px;
    font-weight: 500;
  }
}

.ev-field--grow {
  flex: 1;
  min-width: 160px;
}

.ev-input-with-btn {
  display: flex;
  gap: var(--spacing-xs);

  select,
  input {
    flex: 1;
    min-width: 0;
  }
}

.ev-check {
  display: flex;
  align-items: center;
  gap: var(--spacing-sm);
  min-height: 30px;
  font-size: 13px;
  color: var(--color-text-primary);
  cursor: pointer;

  input {
    width: 16px;
    height: 16px;
    accent-color: var(--color-accent);
  }
}

.ev-help {
  font-size: 11px;
}

.ev-help--warn {
  color: #ffb020;
}

.ev-editor-footer {
  display: flex;
  align-items: center;
  gap: var(--spacing-sm);
  margin-top: var(--spacing-xs);
}

.ev-result {
  font-size: 12px;

  &.ok { color: var(--color-success); }
  &.error { color: #e53e3e; }
}

@keyframes ev-flash {
  0% { filter: brightness(2.2); box-shadow: 0 0 10px 2px #fff; }
  100% { filter: none; box-shadow: none; }
}

@keyframes ev-row-flash {
  0% { background: color-mix(in srgb, var(--color-warning) 45%, transparent); }
  100% { background: transparent; }
}

@keyframes ev-spin {
  from { transform: rotate(0deg); }
  to { transform: rotate(360deg); }
}
</style>

<template>
  <!-- The Playback tab, in the frame every timeline editor here shares
       (assets/styles/timeline.scss): fader | toolbar, timeline, zoom row |
       inspector. tabindex -1: a click on the tab's blank parts lands focus
       here rather than on the page, so Delete still reaches the keyframe
       handler below. -->
  <div class="waveform-trimmer tl-frame" tabindex="-1" @keydown="handleTrimmerKeydown">
    <!-- Volume Control with dB Display -->
    <div class="volume-control-section">
      <div class="volume-label">
        <span class="volume-label-text">
          {{ t('properties.volume') }}
          <FieldHelp :text="t('properties.volumeHelp')" />
        </span>
        <!-- Typed level (#56): the slider is small, and a precise trim is a
             number, not a drag. Commits on Enter/blur, Escape reverts. -->
        <span class="db-entry">
          <input
            type="text"
            inputmode="decimal"
            class="db-input"
            :value="isMixed('volume') ? '' : volumeText"
            :placeholder="isMixed('volume') ? t('properties.multipleValues') : ''"
            :aria-label="t('properties.volumeDbInput')"
            @focus="($event.target as HTMLInputElement).select()"
            @keydown.enter="($event.target as HTMLInputElement).blur()"
            @keydown.esc="cancelVolumeText"
            @change="handleVolumeTextChange"
          />
          <span class="db-unit">dB</span>
        </span>
      </div>
      <div class="volume-slider-container">
        <input
          type="range"
          orient="vertical"
          class="volume-slider-vertical"
          :min="VOLUME_MIN_DB"
          :max="VOLUME_MAX_DB"
          step="0.1"
          :value="volumeDB"
          :aria-label="t('properties.volume')"
          @input="handleVolumeInput"
          @change="handleVolumeDragEnd"
          :style="{ '--volume-handle-color': volumeHandleColor }"
        />
        <div class="volume-markers">
          <span>+10</span>
          <span>0</span>
          <span>-12</span>
          <span>-24</span>
          <span>-∞</span>
        </div>
      </div>
    </div>

    <div class="tl-main">
      <!-- Toolbar: this tab's tools on the left, the cursor and Preview from
           cursor on the right, as on every timeline tab. -->
      <div class="tl-toolbar">
        <Btn icon="content_cut" :text="t('properties.trimSilence')" @click="trimSilence" />
        <Btn icon="tune" :text="t('properties.normalize')" @click="normalizeAudio" />

        <!-- Volume automation (2.6). One item at a time: keyframes are
             positions in that item's own file. -->
        <template v-if="!multiSelect">
          <span class="tl-toolbar-sep" aria-hidden="true" />
          <label class="insp-check automation-toggle">
            <input type="checkbox" :checked="automationEnabled" @change="handleAutomationToggle" />
            <span class="material-symbols-rounded" aria-hidden="true">timeline</span>
            <span>{{ t('automation.enable') }}</span>
            <FieldHelp :text="t('automation.enableHint')" />
          </label>
          <template v-if="automationEnabled">
            <button
              type="button"
              class="tl-icon-btn"
              :disabled="!canAddKeyframe"
              :title="t('automation.addKeyframeHint')"
              :aria-label="t('automation.addKeyframe')"
              @click="addKeyframeAtCursor"
            >
              <span class="material-symbols-rounded" aria-hidden="true">add_circle</span>
            </button>
            <button
              type="button"
              class="tl-icon-btn"
              :disabled="removableIndex === null"
              :title="t('automation.removeKeyframeHint')"
              :aria-label="t('automation.removeKeyframe')"
              @click="removeKeyframe"
            >
              <span class="material-symbols-rounded" aria-hidden="true">remove_circle</span>
            </button>
            <button
              type="button"
              class="tl-icon-btn"
              :disabled="automationPoints.length === 0"
              :title="t('automation.clearHint')"
              :aria-label="t('automation.clear')"
              @click="clearAutomation"
            >
              <span class="material-symbols-rounded" aria-hidden="true">delete_sweep</span>
            </button>
          </template>
        </template>

        <span class="tl-toolbar-spacer" />

        <TimelineCursorTools
          v-if="!multiSelect"
          :item-uuid="audioItem.uuid"
          :cursor="cursorTime"
          :fallback="inPoint"
          :duration="duration"
          @update:cursor="cursorTime = $event"
        />
      </div>

      <!-- Waveform Canvas Container. Kept mounted in ALL modes so the
           ResizeObserver target is stable; only its contents swap between the
           single-item canvas and the multi-selection message. -->
      <div
        class="waveform-container tl-timeline"
        :class="{ 'automation-on': automationEnabled && !multiSelect }"
        ref="waveformContainer"
        tabindex="0"
        :style="{ cursor: canvasCursor }"
        @wheel.prevent="handleWheel"
        @keydown="handleCanvasKeydown"
        @mousedown.capture="handleMiddlePan"
        @auxclick.prevent
      >
        <!-- Multi-selection: the single-item canvas can't represent many items. -->
        <div v-if="multiSelect" class="waveform-multi-message">
          <span class="material-symbols-rounded">layers</span>
          <p>{{ t('properties.multiSelectWaveform') }}</p>
        </div>

        <template v-else>
        <canvas
          ref="waveformCanvas"
          class="waveform-canvas"
          @mousedown="handleCanvasMouseDown"
          @mousemove="handleCanvasHover"
          @mouseleave="hoverTarget = 'none'"
          @contextmenu.prevent="handleCanvasContextMenu"
        ></canvas>

        <span v-if="editReadout" class="tl-readout">{{ editReadout }}</span>

        <!-- Trim Handles -->
        <div
          class="trim-handle trim-handle-in"
          :style="{ left: inPointPosition + 'px' }"
          @mousedown.prevent="startDragHandle('in', $event)"
        >
          <div class="trim-line"></div>
          <div class="trim-grip">
            <span class="material-symbols-rounded">arrow_forward</span>
          </div>
        </div>

        <div
          class="trim-handle trim-handle-out"
          :style="{ left: outPointPosition + 'px' }"
          @mousedown.prevent="startDragHandle('out', $event)"
        >
          <div class="trim-line"></div>
          <div class="trim-grip">
            <span class="material-symbols-rounded">arrow_back</span>
          </div>
        </div>

        <!-- Trim Region Overlay -->
        <div
          class="trim-overlay trim-overlay-left"
          :style="{ width: Math.max(0, inPointPosition) + 'px' }"
        ></div>
        <div
          class="trim-overlay trim-overlay-right"
          :style="{ left: outPointPosition + 'px' }"
        ></div>

        <!-- Fade Handles (hidden for cart items) -->
        <template v-if="!isCartItem">
          <!-- Play Fade Handle (fade in end) -->
          <div
            v-if="playFade > 0"
            class="fade-handle fade-handle-play"
            :style="{ left: playFadePosition + 'px' }"
            @mousedown.prevent="startDragFade('play', $event)"
            :title="t('waveform.playFadeTitle', { seconds: playFade.toFixed(1) })"
          >
            <div class="fade-line fade-line-red"></div>
            <div class="fade-grip fade-grip-red">
              <span class="material-symbols-rounded">trending_up</span>
            </div>
          </div>

          <!-- Stop Fade Handle (fade out start) -->
          <div
            v-if="stopFade > 0"
            class="fade-handle fade-handle-stop"
            :style="{ left: stopFadePosition + 'px' }"
            @mousedown.prevent="startDragFade('stop', $event)"
            :title="t('waveform.stopFadeTitle', { seconds: stopFade.toFixed(1) })"
          >
            <div class="fade-line fade-line-red"></div>
            <div class="fade-grip fade-grip-red">
              <span class="material-symbols-rounded">trending_down</span>
            </div>
          </div>

          <!-- Cross Fade Handle (crossfade start) -->
          <div
            v-if="crossFade > 0"
            class="fade-handle fade-handle-cross"
            :style="{ left: crossFadePosition + 'px' }"
            @mousedown.prevent="startDragFade('cross', $event)"
            :title="t('waveform.crossFadeTitle', { seconds: crossFade.toFixed(1) })"
          >
            <div class="fade-line fade-line-yellow"></div>
            <div class="fade-grip fade-grip-yellow">
              <span class="material-symbols-rounded">swap_horiz</span>
            </div>
          </div>

          <!-- Start Next Marker (radio-style segue point) -->
          <div
            v-if="startNextEnabled"
            class="fade-handle fade-handle-startnext"
            :style="{ left: startNextPosition + 'px' }"
            @mousedown.prevent="startDragFade('startNext', $event)"
            :title="t('waveform.startNextTitle', { time: formatTimeDetailed(startNextTime) })"
          >
            <div class="fade-line fade-line-green"></div>
            <div class="fade-grip fade-grip-green">
              <span class="material-symbols-rounded">skip_next</span>
            </div>
          </div>
        </template>
        </template>
      </div>

      <TimelineNav v-if="!multiSelect" :view="view" />
    </div>

    <!-- The inspector: this cue's playback times, in sections. A cart item
         has no segue or pre-end fades, only the fade its Stop button uses. -->
    <aside class="tl-inspector">
      <!-- Two times to a section, so the whole inspector fits beside the
           waveform at the panel's 300px default with nothing scrolling. -->
      <div class="insp-groups">
        <section class="insp-group">
          <h4 class="insp-heading">{{ t('properties.groupTrim') }}</h4>
          <InspectorTimeField
            :label="t('properties.inPoint')"
            :value="inPoint"
            :mixed="isMixed('inPoint')"
            @set="setInPoint"
            @nudge="adjustInPoint"
          />
          <InspectorTimeField
            :label="t('properties.outPoint')"
            :value="outPoint"
            :mixed="isMixed('outPoint')"
            @set="setOutPoint"
            @nudge="adjustOutPoint"
          />
          <p class="insp-readout">
            {{ t('properties.duration') }}
            <span class="insp-readout-value">{{ multiSelect ? t('properties.multipleValues') : formatTimeDetailed(duration) }}</span>
          </p>
        </section>

        <section v-if="isCartItem" class="insp-group">
          <h4 class="insp-heading">{{ t('properties.groupStopLoop') }}</h4>
          <InspectorTimeField
            :label="t('properties.manualStopFade')"
            :help="t('properties.manualStopFadeHint')"
            :value="manualStopFade"
            :mixed="isMixed('manualStopFade')"
            @set="setManualStopFade"
            @nudge="adjustManualStopFade"
          />
        </section>

        <template v-else>
          <section class="insp-group">
            <h4 class="insp-heading">{{ t('properties.groupFades') }}</h4>
            <InspectorTimeField
              :label="t('properties.playFade')"
              :help="t('properties.playFadeHelp')"
              :value="playFade"
              :mixed="isMixed('playFade')"
              @set="setPlayFade"
              @nudge="adjustPlayFade"
            />
            <InspectorTimeField
              :label="t('properties.stopFade')"
              :help="t('properties.stopFadeHelp')"
              :value="stopFade"
              :mixed="isMixed('stopFade')"
              @set="setStopFade"
              @nudge="adjustStopFade"
            />
          </section>

          <!-- The two ways this cue hands over to the next: a crossfade, or
               Start Next at a marker (Settings' "default transition mode"
               chooses between the same two for new cues). -->
          <section class="insp-group">
            <h4 class="insp-heading">{{ t('properties.groupTransition') }}</h4>
            <InspectorTimeField
              :label="t('properties.crossFade')"
              :help="t('properties.crossFadeHelp')"
              :value="crossFade"
              :mixed="isMixed('crossFade')"
              @set="setCrossFade"
              @nudge="adjustCrossFade"
            />
            <InspectorTimeField
              :help="t('properties.startNextHelp')"
              :value="startNextTime"
              :mixed="isMixed('startNextTime')"
              :disabled="!startNextEnabled"
              @set="setStartNextTime"
              @nudge="adjustStartNextTime"
            >
              <template #label>
                <span class="start-next-toggle">
                  <input
                    type="checkbox"
                    :checked="!isMixed('startNextEnabled') && startNextEnabled"
                    :indeterminate="isMixed('startNextEnabled')"
                    :aria-label="t('properties.startNextEnable')"
                    @change="handleStartNextEnabledChange"
                  />
                  <!-- "Start next at" + the time beside it reads as the setting. -->
                  <span class="insp-label-text">{{ t('properties.startNextTime') }}</span>
                </span>
              </template>
            </InspectorTimeField>
            <label class="insp-check insp-check--sub" :class="{ 'insp-disabled': !startNextEnabled }">
              <input
                type="checkbox"
                :checked="!isMixed('startNextFadeOut') && startNextFadeOut"
                :indeterminate="isMixed('startNextFadeOut')"
                :disabled="!startNextEnabled"
                @change="handleStartNextFadeOutChange"
              />
              <span>{{ t('properties.startNextFadeOut') }}</span>
            </label>
          </section>

          <!-- The operator's own fades: the Stop button, and a loop turning round. -->
          <section class="insp-group">
            <h4 class="insp-heading">{{ t('properties.groupStopLoop') }}</h4>
            <InspectorTimeField
              :label="t('properties.manualStopFade')"
              :help="t('properties.manualStopFadeHint')"
              :value="manualStopFade"
              :mixed="isMixed('manualStopFade')"
              @set="setManualStopFade"
              @nudge="adjustManualStopFade"
            />
            <InspectorTimeField
              :label="t('properties.loopCrossfade')"
              :help="loopCrossfadeHelp"
              :value="loopCrossfade"
              :mixed="isMixed('loopCrossfade')"
              :disabled="!isLooping"
              @set="setLoopCrossfade"
              @nudge="adjustLoopCrossfade"
            />
          </section>
        </template>
      </div>
    </aside>
  </div>
</template>

<script setup lang="ts">
import type { AudioItem, VolumeAutomation, VolumeAutomationPoint } from '~/types/project';
import {
  ENVELOPE_MIN_DB, ENVELOPE_MAX_DB, clampEnvelopeDb, envelopeDbAt, sortPoints, addPoint,
  applyRangeDelta, shiftPoints, tidyPoints, makeCompositeGain,
} from '~/utils/volumeAutomation';
import { manualStopFadeOf } from '~/types/project';
import { calculateNormalizationGain } from '~/utils/audio';
import {
  waveformLanes as lanesOf, drawWaveformLanes, drawPlaybackMarkers, drawTimeRuler, RULER_HEIGHT,
} from '~/utils/waveformRender';
import { formatEventTime } from '~/utils/events';
import { useTimelineView } from '~/composables/useTimelineView';
import Btn from './Btn.vue';
import FieldHelp from './FieldHelp.vue';
import InspectorTimeField from './InspectorTimeField.vue';
import TimelineCursorTools from './TimelineCursorTools.vue';
import TimelineNav from './TimelineNav.vue';
import { useOutputTarget, METER_COLORS } from '~/composables/useOutputTarget';
import { useLiveplayServer } from '~/composables/useLiveplayServer';
import { focusTakesKeys } from '~/utils/hotkeyGuard';

const props = defineProps<{
  audioItem: AudioItem;
  // True when more than one item is selected. The single-item waveform canvas
  // and the out-point control are meaningless across a heterogeneous selection
  // (each item has its own duration), so they're replaced/disabled. Crucially
  // this ALSO suppresses the self-healing waveform re-request below: with many
  // items selected that re-request fires per displayed anchor and races the
  // batch edit, which is what made auto-trim / auto-volume revert.
  multiSelect?: boolean;
  /**
   * Which of these fields differ across the selection. A mixed field shows
   * "Multiple values" until it is set, and setting it sets every selected cue.
   */
  mixed?: Record<string, boolean>;
}>();
const isMixed = (field: string) => !!props.mixed?.[field];

const emit = defineEmits<{
  'update:volume': [value: number];
  'update:inPoint': [value: number];
  'update:outPoint': [value: number];
  'update:playFade': [value: number];
  'update:stopFade': [value: number];
  'update:pauseFade': [value: number];
  'update:crossFade': [value: number];
  'update:manualStopFade': [value: number];
  'update:loopCrossfade': [value: number];
  'update:startNextEnabled': [value: boolean];
  'update:startNextTime': [value: number];
  'update:startNextFadeOut': [value: boolean];
  'update:volumeAutomation': [value: VolumeAutomation];
  'change': [];
  'normalize': [];
  'trimSilence': [];
}>();

const { t } = useLocalization();
const { colorForLevel, levels: outputTargetLevels } = useOutputTarget();

// Parse a CSS hex color ("#rrggbb") into [r, g, b] without DOM tricks.
function hexToRgb(hex: string): [number, number, number] {
  const n = parseInt(hex.replace('#', ''), 16);
  return [(n >> 16) & 0xff, (n >> 8) & 0xff, n & 0xff];
}

// Get audio engine for playback position
const { activeCues, seekCue } = useAudioEngine();

// Check if this is a cart item
const isCartItem = computed(() => {
  return props.audioItem.index && props.audioItem.index.length > 0 && props.audioItem.index[0] === -1;
});

// Fade values
const playFade = computed(() => props.audioItem.playFade || 0);
const stopFade = computed(() => props.audioItem.stopFade || 0);
const crossFade = computed(() => props.audioItem.crossFade || 0);
const manualStopFade = computed(() => manualStopFadeOf(props.audioItem));
const loopCrossfade = computed(() => props.audioItem.loopCrossfade || 0);
const isLooping = computed(() => props.audioItem.endBehavior?.action === 'loop');

// Fades have no fixed ceiling (#56): a scene-change ambience can fade over
// minutes. The one real bound is the audio itself — a fade into or out of the
// trimmed region cannot be longer than the region.
const trimmedLength = computed(() => Math.max(0, outPoint.value - inPoint.value));
const clampFade = (v: number) => Math.max(0, Math.min(v, trimmedLength.value));

// Start Next marker (absolute seconds within the file)
const startNextEnabled = computed(() => !!props.audioItem.startNextEnabled);
const startNextTime = computed(() => props.audioItem.startNextTime || 0);
const startNextFadeOut = computed(() => !!props.audioItem.startNextFadeOut);

// Refs
const waveformCanvas = ref<HTMLCanvasElement | null>(null);
const waveformContainer = ref<HTMLDivElement | null>(null);
const isDrawing = ref(false);


// Get playback position for playhead
const playbackPosition = computed(() => {
  const cue = activeCues.value.get(props.audioItem.uuid);
  if (!cue) return null;

  // Prefer the absolute file playhead reported by the server — it's independent
  // of the in point, so dragging in/out while playing doesn't shift the line.
  if (typeof cue.playheadSeconds === 'number') return cue.playheadSeconds;

  // Fallback: currentTime is relative to inPoint, so add it back for absolute.
  const inPoint = props.audioItem.inPoint || 0;
  return cue.currentTime + inPoint;
});

// Use existing waveform data from audioItem. `waveformData` is the combined
// trace (per-bucket max across channels) used for analysis — auto-trim, RMS,
// normalisation — so a stereo file is measured from BOTH channels (#47).
const waveformData = computed(() => props.audioItem?.waveform?.peaks ?? null);
const hasWaveform = computed(() => waveformData.value && waveformData.value.length > 0);

// Lanes to draw (see utils/waveformRender): stereo renders L above R.
const waveformLanes = computed<number[][]>(() => lanesOf(props.audioItem?.waveform));

// ---- Self-healing waveform regeneration -------------------------------------
// Occasionally an item ends up with no waveform (e.g. a server re-sync that
// stripped peaks before the client cache had seen them). Rather than make the
// user press "Regenerate waveform", request it from the server automatically
// whenever the panel shows an item with no peaks. The result arrives as a
// waveform_ready doc_patch which sets audioItem.waveform and redraws.
const { currentProject } = useProject();

// Resolve the server-side absolute media path, falling back to project folder +
// relative mediaPath for legacy items (mirrors the manual regenerate button).
const resolveMediaPath = (): string => {
  const it = props.audioItem;
  if (!it) return '';
  if (it.mediaServerPath) return it.mediaServerPath;
  const folder = currentProject.value?.folderPath || '';
  if (it.mediaPath && folder) {
    const rel = it.mediaPath.replace(/^[\\/]+/, '');
    return `${folder.replace(/[\\/]+$/, '')}/${rel}`;
  }
  return '';
};

// Guard so we don't spam the server while a generation is in flight. Reset
// when the item changes or once a waveform actually arrives.
let waveformRequestedFor: string | null = null;
// A property edit briefly round-trips the item through the server (PATCH →
// item_updated echo), during which the local waveform can momentarily read as
// absent before `restoreWaveform` re-attaches it from the session cache. Firing
// a forced server regen in that window is wasteful (and used to cause the
// values to snap back). So we DEFER the self-heal and only act if the waveform
// is still genuinely missing after the echo has had time to land.
let ensureWaveformTimer: ReturnType<typeof setTimeout> | null = null;
const SELF_HEAL_DELAY_MS = 600;

const requestWaveformNow = () => {
  const it = props.audioItem;
  if (!it || it.type !== 'audio') return;
  if (props.multiSelect) return;
  if (hasWaveform.value) { waveformRequestedFor = null; return; }
  if (waveformRequestedFor === it.uuid) return;
  const path = resolveMediaPath();
  if (!path) return;
  waveformRequestedFor = it.uuid;
  useLiveplayServer().requestWaveformGeneration(path, it.uuid, true)
    .catch(() => { /* best-effort — the manual button remains as a fallback */ });
};

const ensureWaveform = () => {
  const it = props.audioItem;
  if (!it || it.type !== 'audio') return;
  // Don't self-heal while multiple items are selected — the canvas isn't shown,
  // and a forced regen here echoes back a waveform_ready that can clobber the
  // values a multi-item auto-trim / auto-volume just applied.
  if (props.multiSelect) return;
  if (hasWaveform.value) { waveformRequestedFor = null; return; }
  // Debounce: re-check after the echo window. A transient drop heals itself and
  // this becomes a no-op; only a real, persistent gap reaches the server.
  if (ensureWaveformTimer) clearTimeout(ensureWaveformTimer);
  ensureWaveformTimer = setTimeout(() => { ensureWaveformTimer = null; requestWaveformNow(); }, SELF_HEAL_DELAY_MS);
};

// React to the panel switching items, or a waveform appearing/disappearing.
// A new item is missing immediately and persistently, so request it without the
// settle delay; a disappearance goes through the debounced ensureWaveform.
watch(() => props.audioItem?.uuid, () => {
  if (ensureWaveformTimer) { clearTimeout(ensureWaveformTimer); ensureWaveformTimer = null; }
  waveformRequestedFor = null;
  if (!hasWaveform.value) ensureWaveform();
});
watch(hasWaveform, (has) => {
  if (!has) ensureWaveform();
  else {
    waveformRequestedFor = null;
    if (ensureWaveformTimer) { clearTimeout(ensureWaveformTimer); ensureWaveformTimer = null; }
  }
});

// Volume in dB. The slider's floor is silence; +10 dB matches what Normalize
// may apply.
const VOLUME_MIN_DB = -60;
const VOLUME_MAX_DB = 10;
const volumeDB = computed({
  get: () => {
    // Convert linear volume (0-2+) to dB
    const linear = props.audioItem?.volume ?? 1;
    if (linear <= 0) return VOLUME_MIN_DB; // -infinity
    return 20 * Math.log10(linear);
  },
  set: (db: number) => {
    const linear = db <= VOLUME_MIN_DB ? 0 : Math.pow(10, db / 20);
    emit('update:volume', linear);
  }
});

// Silence reads "-∞", as the keyframe readout on the same tab writes it; the
// "dB" beside the field completes it.
const volumeText = computed(() => (volumeDB.value <= VOLUME_MIN_DB ? '-∞' : volumeDB.value.toFixed(1)));

// Typed dB level. Accepts "-6", "-6.5 dB", "+3", or "-∞" / "-inf" for silence.
const handleVolumeTextChange = (event: Event) => {
  const input = event.target as HTMLInputElement;
  const raw = input.value.trim().toLowerCase().replace(/db$/, '').trim();
  const db = /^-?(inf|∞)/.test(raw) ? VOLUME_MIN_DB : parseFloat(raw);
  if (Number.isNaN(db)) {
    input.value = volumeText.value;
    return;
  }
  volumeDB.value = Math.max(VOLUME_MIN_DB, Math.min(VOLUME_MAX_DB, db));
  emit('change');
};
const cancelVolumeText = (event: KeyboardEvent) => {
  const input = event.target as HTMLInputElement;
  input.value = volumeText.value;
  input.blur();
};

// Compute handle color based on dB level
const volumeHandleColor = computed(() => {
  const db = volumeDB.value;
  if (db > 0) return '#991b1b';      // Dark red above 0dB
  if (db > -1) return '#dc2626';     // Red 0 to -1dB
  if (db > -6) return '#eab308';     // Yellow -1 to -6dB
  if (db > -18) return '#16a34a';    // Green -6 to -18dB
  if (db > -36) return '#22c55e';    // Dark green -18 to -36dB
  return '#1a4d2e';                  // Darker green below -36dB
});

// The edit cursor: where Add Keyframe and Preview from Cursor act, set by a
// click on the waveform or typed in the toolbar. Only drawn while the cue is
// stopped; a playing cue has its playhead instead.
const cursorTime = ref<number | null>(null);

// Time values
const inPoint = computed(() => props.audioItem?.inPoint ?? 0);
const outPoint = computed(() => props.audioItem?.outPoint ?? props.audioItem?.duration ?? 0);
// Use waveform's own duration as fallback — audioItem.duration can be 0 if
// metadata hasn't arrived yet, but the waveform object always carries duration.
const duration = computed(() =>
  props.audioItem?.duration || props.audioItem?.waveform?.duration || 0
);

// Canvas dimensions - use reactive ref to ensure handle positions update on resize
const containerWidth = ref(800);
const canvasWidth = computed(() => containerWidth.value);

// MEASURED, not a constant. The properties panel is resizable, so the drawing
// surface has to follow the box it is drawn into: with a fixed 120 the bitmap
// was scaled by CSS to whatever height the container actually had, which is
// what made the waveform read as blurred and squashed. Seeded with the old
// constant so the first draw — before the container has been measured — is
// sane rather than zero-height.
const containerHeight = ref(120);

// The visible window (composables/useTimelineView), the same model and
// controls as the Events tab: zoom to a quarter of a second, a log slider,
// the wheel zooming at the pointer and Shift+wheel or a middle drag panning.
// The buttons zoom around the edit cursor when it is on screen.
const view = useTimelineView(duration, () => cursorTime.value);
const visibleDuration = computed(() => view.visibleDuration || duration.value || 1);
const visibleStart = computed(() => view.viewStart);
const visibleEnd = computed(() => Math.min(duration.value, view.viewEnd));
// Another cue: start from the whole file.
watch(() => props.audioItem?.uuid, () => view.fit());

// Position calculations for trim handles
const inPointPosition = computed(() => {
  const relativeTime = inPoint.value - visibleStart.value;
  return (relativeTime / visibleDuration.value) * canvasWidth.value;
});

const outPointPosition = computed(() => {
  const relativeTime = outPoint.value - visibleStart.value;
  return (relativeTime / visibleDuration.value) * canvasWidth.value;
});

// Fade handle positions (respecting trim points)
const playFadePosition = computed(() => {
  const fadeEndTime = inPoint.value + playFade.value;
  const relativeTime = fadeEndTime - visibleStart.value;
  return (relativeTime / visibleDuration.value) * canvasWidth.value;
});

const stopFadePosition = computed(() => {
  const fadeStartTime = outPoint.value - stopFade.value;
  const relativeTime = fadeStartTime - visibleStart.value;
  return (relativeTime / visibleDuration.value) * canvasWidth.value;
});

const crossFadePosition = computed(() => {
  const crossFadeStartTime = outPoint.value - crossFade.value;
  const relativeTime = crossFadeStartTime - visibleStart.value;
  return (relativeTime / visibleDuration.value) * canvasWidth.value;
});

const startNextPosition = computed(() => {
  const relativeTime = startNextTime.value - visibleStart.value;
  return (relativeTime / visibleDuration.value) * canvasWidth.value;
});

// Seek to position (when clicking waveform)
const seekToPosition = (absoluteTime: number) => {
  const cue = activeCues.value.get(props.audioItem.uuid);
  if (!cue) return;

  const inPoint = props.audioItem.inPoint || 0;
  const clampedTime = Math.max(
    inPoint,
    Math.min(absoluteTime, props.audioItem.outPoint || props.audioItem.duration)
  );

  // Routes to the server (server seek is a no-op for now and logs a warning;
  // wire-up will land with the engine's seek endpoint).
  seekCue(props.audioItem.uuid, clampedTime);
};

// Handle dragging
const dragState = ref<{ handle: 'in' | 'out' | 'play' | 'stop' | 'cross' | 'startNext' | null; startX: number; startValue: number }>({
  handle: null,
  startX: 0,
  startValue: 0
});

const startDragHandle = (handle: 'in' | 'out', event: MouseEvent) => {
  dragState.value = {
    handle,
    startX: event.clientX,
    startValue: handle === 'in' ? inPoint.value : outPoint.value
  };

  const handleMouseMove = (e: MouseEvent) => {
    if (!dragState.value.handle) return;

    const deltaX = e.clientX - dragState.value.startX;
    const deltaTime = (deltaX / canvasWidth.value) * visibleDuration.value;
    const newValue = Math.max(0, Math.min(duration.value, dragState.value.startValue + deltaTime));

    if (dragState.value.handle === 'in') {
      emit('update:inPoint', Math.min(newValue, outPoint.value - 0.01));
    } else if (dragState.value.handle === 'out') {
      emit('update:outPoint', Math.max(newValue, inPoint.value + 0.01));
    }
  };

  const handleMouseUp = () => {
    if (dragState.value.handle) {
      emit('change');
    }
    dragState.value.handle = null;
    document.removeEventListener('mousemove', handleMouseMove);
    document.removeEventListener('mouseup', handleMouseUp);
  };

  document.addEventListener('mousemove', handleMouseMove);
  document.addEventListener('mouseup', handleMouseUp);
};

// Handle fade dragging
const startDragFade = (fadeType: 'play' | 'stop' | 'cross' | 'startNext', event: MouseEvent) => {
  const currentValue = fadeType === 'play' ? playFade.value
    : fadeType === 'stop' ? stopFade.value
    : fadeType === 'cross' ? crossFade.value
    : startNextTime.value;

  dragState.value = {
    handle: fadeType,
    startX: event.clientX,
    startValue: currentValue
  };

  const handleMouseMove = (e: MouseEvent) => {
    if (!dragState.value.handle) return;

    const deltaX = e.clientX - dragState.value.startX;
    const deltaTime = (deltaX / canvasWidth.value) * visibleDuration.value;

    if (dragState.value.handle === 'play') {
      // Play fade: drag right increases fade duration
      emit('update:playFade', clampFade(dragState.value.startValue + deltaTime));
    } else if (dragState.value.handle === 'stop') {
      // Stop fade: drag left increases fade duration (moving the start point earlier)
      emit('update:stopFade', clampFade(dragState.value.startValue - deltaTime));
    } else if (dragState.value.handle === 'cross') {
      // Cross fade: drag left increases fade duration (moving the start point earlier)
      emit('update:crossFade', clampFade(dragState.value.startValue - deltaTime));
    } else if (dragState.value.handle === 'startNext') {
      // Start Next marker: absolute position, clamped to the trimmed region.
      const newValue = Math.max(inPoint.value, Math.min(outPoint.value, dragState.value.startValue + deltaTime));
      emit('update:startNextTime', newValue);
    }
  };

  const handleMouseUp = () => {
    if (dragState.value.handle) {
      emit('change');
    }
    dragState.value.handle = null;
    document.removeEventListener('mousemove', handleMouseMove);
    document.removeEventListener('mouseup', handleMouseUp);
  };

  document.addEventListener('mousemove', handleMouseMove);
  document.addEventListener('mouseup', handleMouseUp);
};

// ---- Volume automation (2.6) ----------------------------------------------
// The envelope is drawn over the waveform on a linear dB axis, -inf (bottom)
// to +10 dB (top). Editing, all on the canvas:
//   * click the line            → add a keyframe there
//   * drag the line             → move the keyframes either side of that stretch
//   * drag a keyframe           → move it (time between its neighbours, and level)
//   * right-click a keyframe    → remove it (Delete removes the selected one)
//   * drag empty waveform       → select a range; drag the line inside it to
//                                 change that whole section, with ramps inside
//                                 its edges (applyRangeDelta)
//   * click empty waveform      → the edit cursor (stopped) / seek (playing)
// Shift while dragging = fine (a tenth of the movement).
const automationEnabled = computed(() => !!props.audioItem?.volumeAutomation?.enabled);
const automationPoints = computed<VolumeAutomationPoint[]>(() =>
  sortPoints(props.audioItem?.volumeAutomation?.points ?? []));

const selection = ref<{ a: number; b: number } | null>(null);
const selectedKeyframe = ref<number | null>(null);   // index into automationPoints
type HoverKind = 'none' | 'keyframe' | 'line' | 'selection-line' | 'background';
const hoverTarget = ref<HoverKind>('none');
const isPlaying = computed(() => playbackPosition.value !== null);

watch(() => props.audioItem?.uuid, () => {
  cursorTime.value = null;
  selection.value = null;
  selectedKeyframe.value = null;
});
watch(automationEnabled, (on) => {
  if (!on) { selection.value = null; selectedKeyframe.value = null; }
});

const commitAutomation = (points: VolumeAutomationPoint[], final = false, enabled = automationEnabled.value) => {
  emit('update:volumeAutomation', { enabled, points: final ? tidyPoints(points) : points });
  if (final) emit('change');
};

const handleAutomationToggle = (event: Event) => {
  const on = (event.target as HTMLInputElement).checked;
  // Unticking only bypasses: the keyframes stay in the item for next time.
  commitAutomation(automationPoints.value, true, on);
};

// Where Add Keyframe lands: the playhead while playing, else the cursor.
const editPosition = computed<number | null>(() =>
  isPlaying.value ? playbackPosition.value : cursorTime.value);
const canAddKeyframe = computed(() => automationEnabled.value && editPosition.value !== null);

const addKeyframeAtCursor = () => {
  const t = editPosition.value;
  if (t === null) return;
  const pts = addPoint(automationPoints.value, t, envelopeDbAt(automationPoints.value, t));
  commitAutomation(pts, true);
  selectedKeyframe.value = tidyPoints(pts).findIndex(p => Math.abs(p.t - t) < 0.001);
};

// A keyframe within a few pixels of the cursor counts as "at the cursor".
const secondsPerPixel = () => visibleDuration.value / Math.max(1, canvasWidth.value);
const removableIndex = computed<number | null>(() => {
  if (!automationEnabled.value) return null;
  if (selectedKeyframe.value !== null && selectedKeyframe.value < automationPoints.value.length)
    return selectedKeyframe.value;
  const t = editPosition.value;
  if (t === null) return null;
  const tol = 4 * secondsPerPixel();
  const i = automationPoints.value.findIndex(p => Math.abs(p.t - t) <= tol);
  return i >= 0 ? i : null;
});

const removeKeyframeAt = (index: number) => {
  const pts = automationPoints.value.filter((_, i) => i !== index);
  selectedKeyframe.value = null;
  commitAutomation(pts, true);
};
const removeKeyframe = () => {
  if (removableIndex.value !== null) removeKeyframeAt(removableIndex.value);
};

const clearAutomation = () => {
  if (automationPoints.value.length === 0) return;
  if (!window.confirm(t('automation.clearConfirm'))) return;
  selectedKeyframe.value = null;
  selection.value = null;
  commitAutomation([], true);
};

const formatDb = (db: number) =>
  db <= ENVELOPE_MIN_DB ? '-∞ dB' : `${db > 0 ? '+' : ''}${db.toFixed(1)} dB`;

const editReadout = computed(() => {
  if (props.multiSelect) return '';
  const k = selectedKeyframe.value;
  const p = k !== null ? automationPoints.value[k] : undefined;
  if (p) return `${formatTimeDetailed(p.t)} · ${formatDb(p.db)}`;
  if (selection.value && automationEnabled.value)
    return `${formatTimeDetailed(selection.value.a)} – ${formatTimeDetailed(selection.value.b)}`;
  return '';
});

// ---- Geometry ---------------------------------------------------------------
const ENV_PAD = 8;   // px kept clear above +10 dB and below -inf
// The ruler band along the top (utils/waveformRender) is not drawing area: the
// waveform, fades and envelope all live in the box under it.
const CONTENT_TOP = RULER_HEIGHT;
const timeAtX = (x: number) => visibleStart.value + (x / Math.max(1, canvasWidth.value)) * visibleDuration.value;
const xAtTime = (time: number) => ((time - visibleStart.value) / visibleDuration.value) * canvasWidth.value;
const yAtDb = (db: number, h = containerHeight.value) => {
  const f = (clampEnvelopeDb(db) - ENVELOPE_MIN_DB) / (ENVELOPE_MAX_DB - ENVELOPE_MIN_DB);
  return CONTENT_TOP + ENV_PAD + (1 - f) * Math.max(1, h - CONTENT_TOP - 2 * ENV_PAD);
};
const dbPerPixel = () => (ENVELOPE_MAX_DB - ENVELOPE_MIN_DB) / Math.max(1, containerHeight.value - CONTENT_TOP - 2 * ENV_PAD);
const clampTime = (time: number) => Math.max(0, Math.min(duration.value, time));

type Hit =
  | { kind: 'keyframe'; index: number; t: number }
  | { kind: 'line' | 'selection-line' | 'background'; t: number };

const KEYFRAME_HIT_PX = 7;
const LINE_HIT_PX = 6;
const hitTest = (x: number, y: number): Hit => {
  const t = clampTime(timeAtX(x));
  if (automationEnabled.value && !props.multiSelect) {
    const pts = automationPoints.value;
    let best = -1, bestDist = KEYFRAME_HIT_PX;
    pts.forEach((p, i) => {
      const d = Math.hypot(xAtTime(p.t) - x, yAtDb(p.db) - y);
      if (d <= bestDist) { best = i; bestDist = d; }
    });
    if (best >= 0) return { kind: 'keyframe', index: best, t: pts[best]!.t };
    if (Math.abs(yAtDb(envelopeDbAt(pts, t)) - y) <= LINE_HIT_PX) {
      const sel = selection.value;
      return { kind: sel && t >= sel.a && t <= sel.b ? 'selection-line' : 'line', t };
    }
  }
  return { kind: 'background', t };
};

// Middle-button drag pans a zoomed-in waveform, wherever it starts — over a
// trim or fade handle too (caught in the capture phase, so they never see it),
// as on the Events tab.
const panning = ref(false);
const handleMiddlePan = (event: MouseEvent) => {
  if (event.button !== 1 || props.multiSelect) return;
  event.preventDefault();      // no browser autoscroll
  event.stopPropagation();
  if (view.maxViewStart <= 0) return;   // not zoomed in: nothing to pan
  const fromX = event.clientX;
  const fromStart = view.viewStart;
  panning.value = true;
  const onMove = (e: MouseEvent) => {
    const dt = ((e.clientX - fromX) / Math.max(1, canvasWidth.value)) * visibleDuration.value;
    view.scrollTo(fromStart - dt);
  };
  const onUp = () => {
    panning.value = false;
    document.removeEventListener('mousemove', onMove);
    document.removeEventListener('mouseup', onUp);
  };
  document.addEventListener('mousemove', onMove);
  document.addEventListener('mouseup', onUp);
};

const canvasCursor = computed(() => {
  if (panning.value) return 'grabbing';
  switch (hoverTarget.value) {
    case 'keyframe': return 'grab';
    case 'line':
    case 'selection-line': return 'ns-resize';
    default: return 'crosshair';
  }
});

const handleCanvasHover = (event: MouseEvent) => {
  if (event.buttons) return;   // a drag is in charge
  const rect = waveformCanvas.value?.getBoundingClientRect();
  if (!rect) return;
  hoverTarget.value = hitTest(event.clientX - rect.left, event.clientY - rect.top).kind;
};

// The keyframes a drag on the line moves: the two either side of that
// stretch, or the end one for the held level before the first / after the
// last. With no keyframes the line is one flat level, so the drag makes one.
const segmentFor = (base: VolumeAutomationPoint[], time: number) => {
  if (base.length === 0) return { points: [{ t: time, db: 0 }], indices: [0] };
  let i = -1;
  for (let k = 0; k < base.length; k++) if (base[k]!.t <= time) i = k;
  if (i < 0) return { points: base, indices: [0] };
  if (i >= base.length - 1) return { points: base, indices: [base.length - 1] };
  return { points: base, indices: [i, i + 1] };
};

const DRAG_THRESHOLD_PX = 3;

const handleCanvasMouseDown = (event: MouseEvent) => {
  if (dragState.value.handle) return;
  if (event.button !== 0) return;
  const rect = waveformCanvas.value?.getBoundingClientRect();
  if (!rect) return;
  waveformContainer.value?.focus({ preventScroll: true });

  const x0 = event.clientX - rect.left;
  const y0 = event.clientY - rect.top;
  const hit = hitTest(x0, y0);
  const base = automationPoints.value.map(p => ({ ...p }));
  const sel = selection.value ? { ...selection.value } : null;
  const segment = hit.kind === 'line' ? segmentFor(base, hit.t) : null;
  let latest: VolumeAutomationPoint[] | null = null;
  let moved = false;

  if (hit.kind === 'keyframe') selectedKeyframe.value = hit.index;

  const onMove = (e: MouseEvent) => {
    const x = e.clientX - rect.left;
    const y = e.clientY - rect.top;
    if (!moved && Math.hypot(x - x0, y - y0) < DRAG_THRESHOLD_PX) return;
    moved = true;
    const scale = e.shiftKey ? 0.1 : 1;
    const deltaDb = -(y - y0) * dbPerPixel() * scale;

    if (hit.kind === 'keyframe') {
      const i = hit.index;
      const p = base[i]!;
      const minT = i > 0 ? base[i - 1]!.t + 0.001 : 0;
      const maxT = i < base.length - 1 ? base[i + 1]!.t - 0.001 : duration.value;
      const dt = (x - x0) * secondsPerPixel() * scale;
      const next = base.slice();
      next[i] = { t: Math.max(minT, Math.min(maxT, p.t + dt)), db: clampEnvelopeDb(p.db + deltaDb) };
      latest = next;
      commitAutomation(next);
    } else if (hit.kind === 'line' && segment) {
      latest = shiftPoints(segment.points, segment.indices, deltaDb);
      commitAutomation(latest);
    } else if (hit.kind === 'selection-line' && sel) {
      latest = applyRangeDelta(base, sel.a, sel.b, deltaDb);
      commitAutomation(latest);
    } else if (hit.kind === 'background' && automationEnabled.value) {
      const t2 = clampTime(timeAtX(x));
      selection.value = { a: Math.min(hit.t, t2), b: Math.max(hit.t, t2) };
      selectedKeyframe.value = null;
    }
  };

  const onUp = () => {
    document.removeEventListener('mousemove', onMove);
    document.removeEventListener('mouseup', onUp);
    if (moved) {
      if (latest) commitAutomation(latest, true);
      if (selection.value && selection.value.b - selection.value.a < 0.005) selection.value = null;
      return;
    }
    // A click, not a drag.
    if (hit.kind === 'line' || hit.kind === 'selection-line') {
      const pts = addPoint(base, hit.t, envelopeDbAt(base, hit.t));
      commitAutomation(pts, true);
      selectedKeyframe.value = tidyPoints(pts).findIndex(p => Math.abs(p.t - hit.t) < 0.001);
    } else if (hit.kind === 'background') {
      selection.value = null;
      selectedKeyframe.value = null;
      if (isPlaying.value) seekToPosition(hit.t);
      else cursorTime.value = hit.t;
    }
  };

  document.addEventListener('mousemove', onMove);
  document.addEventListener('mouseup', onUp);
};

const handleCanvasContextMenu = (event: MouseEvent) => {
  const rect = waveformCanvas.value?.getBoundingClientRect();
  if (!rect) return;
  const hit = hitTest(event.clientX - rect.left, event.clientY - rect.top);
  if (hit.kind === 'keyframe') removeKeyframeAt(hit.index);
};

// Delete on the waveform is about the waveform: it removes the selected
// keyframe, and never falls through to the workspace's "delete this cue".
// Escape clears a selection AND goes on to Stop All: a selection is state the
// operator may have forgotten, and the first Esc of a panic must never be
// spent on it (R2-15).
const handleCanvasKeydown = (event: KeyboardEvent) => {
  if (event.key === 'Delete' || event.key === 'Backspace') {
    event.preventDefault();
    event.stopPropagation();
    if (selectedKeyframe.value !== null) removeKeyframeAt(selectedKeyframe.value);
  } else if (event.key === 'Escape') {
    selection.value = null;
    selectedKeyframe.value = null;
  }
};

// Anywhere else on the tab (a tool button, the bar around it) the same: a
// selected keyframe owns Delete, and with none it still doesn't fall through
// to deleting the cue being edited. Fields keep it for typing.
const handleTrimmerKeydown = (event: KeyboardEvent) => {
  if (event.key !== 'Delete' && event.key !== 'Backspace') return;
  if (focusTakesKeys(event)) return;
  event.preventDefault();
  event.stopPropagation();
  if (selectedKeyframe.value !== null) removeKeyframeAt(selectedKeyframe.value);
};

// The envelope's own colour. Not the accent: that is red by default (and any
// colour the operator picks), and red, yellow and green already mean the fades,
// the crossfade and Start Next on this canvas. Fuchsia clashes with none of
// them or the meter-zone bars; a deeper shade keeps contrast on the light theme.
const envelopeColor = () => {
  const bg = getComputedStyle(document.documentElement).getPropertyValue('--color-background').trim();
  const m = /^#?([0-9a-f]{6})$/i.exec(bg);
  const n = m ? parseInt(m[1]!, 16) : 0;
  const luma = 0.299 * ((n >> 16) & 255) + 0.587 * ((n >> 8) & 255) + 0.114 * (n & 255);
  return luma > 140 ? '#a21caf' : '#e879f9';
};

// Drawn over the waveform, under the playhead: the selection, the cursor and
// (when automation is on) the envelope with its keyframes.
const drawAutomationLayer = (ctx: CanvasRenderingContext2D, h: number) => {
  const w = canvasWidth.value;
  const accent = envelopeColor();

  if (automationEnabled.value && selection.value) {
    const xa = Math.max(0, xAtTime(selection.value.a));
    const xb = Math.min(w, xAtTime(selection.value.b));
    if (xb > xa) {
      ctx.save();
      ctx.globalAlpha = 0.14;
      ctx.fillStyle = accent;
      ctx.fillRect(xa, CONTENT_TOP, xb - xa, h - CONTENT_TOP);
      ctx.globalAlpha = 0.6;
      ctx.fillRect(xa, CONTENT_TOP, 1, h - CONTENT_TOP);
      ctx.fillRect(xb - 1, CONTENT_TOP, 1, h - CONTENT_TOP);
      ctx.restore();
    }
  }

  if (!isPlaying.value && cursorTime.value !== null) {
    const x = xAtTime(cursorTime.value);
    if (x >= 0 && x <= w) {
      ctx.save();
      ctx.strokeStyle = 'rgba(255, 255, 255, 0.75)';
      ctx.lineWidth = 1;
      ctx.setLineDash([4, 3]);
      ctx.beginPath(); ctx.moveTo(x + 0.5, 0); ctx.lineTo(x + 0.5, h); ctx.stroke();
      ctx.setLineDash([]);
      ctx.fillStyle = 'rgba(255, 255, 255, 0.85)';
      ctx.beginPath(); ctx.moveTo(x - 5, 0); ctx.lineTo(x + 5, 0); ctx.lineTo(x, 7); ctx.closePath(); ctx.fill();
      ctx.restore();
    }
  }

  if (!automationEnabled.value) return;
  const pts = automationPoints.value;
  ctx.save();
  // 0 dB guide: the item's own volume, where an untouched envelope sits.
  const y0 = yAtDb(0, h);
  ctx.strokeStyle = 'rgba(128, 128, 128, 0.35)';
  ctx.setLineDash([2, 4]);
  ctx.lineWidth = 1;
  ctx.beginPath(); ctx.moveTo(0, y0); ctx.lineTo(w, y0); ctx.stroke();
  ctx.setLineDash([]);

  // The line. Straight between keyframes: the axis is linear in dB, and so is
  // the interpolation.
  ctx.strokeStyle = accent;
  ctx.lineWidth = 2;
  ctx.beginPath();
  ctx.moveTo(0, yAtDb(envelopeDbAt(pts, visibleStart.value), h));
  for (const p of pts) {
    if (p.t <= visibleStart.value || p.t >= visibleEnd.value) continue;
    ctx.lineTo(xAtTime(p.t), yAtDb(p.db, h));
  }
  ctx.lineTo(w, yAtDb(envelopeDbAt(pts, visibleEnd.value), h));
  ctx.stroke();

  // Keyframes.
  pts.forEach((p, i) => {
    const x = xAtTime(p.t);
    if (x < -8 || x > w + 8) return;
    const selected = i === selectedKeyframe.value;
    ctx.beginPath();
    ctx.arc(x, yAtDb(p.db, h), selected ? 5.5 : 4, 0, Math.PI * 2);
    ctx.fillStyle = selected ? '#ffffff' : accent;
    ctx.fill();
    ctx.lineWidth = selected ? 2 : 1.5;
    ctx.strokeStyle = selected ? accent : 'rgba(0, 0, 0, 0.6)';
    ctx.stroke();
  });
  ctx.restore();
};

// Wheel: zoom at the pointer; Shift+wheel or a sideways wheel pans.
const handleWheel = (event: WheelEvent) => {
  if (props.multiSelect) return; // no canvas to zoom in multi-selection
  const rect = waveformContainer.value?.getBoundingClientRect();
  if (!rect) return;
  view.onWheel(event, clampTime(timeAtX(event.clientX - rect.left)), canvasWidth.value);
};

// Real-time volume preview during drag — updates the value but does NOT save.
const handleVolumeInput = (event: Event) => {
  const db = parseFloat((event.target as HTMLInputElement).value);
  if (!isNaN(db)) volumeDB.value = db;
};

// Fires once when the drag ends (mouseup/touchend) — this is when we save.
const handleVolumeDragEnd = () => {
  emit('change');
};

// Times read m:ss.mmm, as on the Events tab and everywhere else in the frame
// (InspectorTimeField parses what is typed).
const formatTimeDetailed = (seconds: number): string => formatEventTime(seconds);

// Typed times (InspectorTimeField has parsed them), clamped as a drag is.
const setInPoint = (s: number) => {
  emit('update:inPoint', Math.max(0, Math.min(s, outPoint.value - 0.01)));
  emit('change');
};

const setOutPoint = (s: number) => {
  emit('update:outPoint', Math.min(props.audioItem.duration, Math.max(s, inPoint.value + 0.01)));
  emit('change');
};

// Adjustment functions for increment/decrement buttons
const adjustInPoint = (delta: number) => {
  const newValue = Math.max(0, Math.min(inPoint.value + delta, outPoint.value - 0.01));
  emit('update:inPoint', newValue);
  emit('change');
};

const adjustOutPoint = (delta: number) => {
  const newValue = Math.min(props.audioItem.duration, Math.max(outPoint.value + delta, inPoint.value + 0.01));
  emit('update:outPoint', newValue);
  emit('change');
};

const adjustPlayFade = (delta: number) => {
  emit('update:playFade', clampFade(playFade.value + delta));
  emit('change');
};

const adjustStopFade = (delta: number) => {
  emit('update:stopFade', clampFade(stopFade.value + delta));
  emit('change');
};

const adjustCrossFade = (delta: number) => {
  emit('update:crossFade', clampFade(crossFade.value + delta));
  emit('change');
};

const setPlayFade = (s: number) => { emit('update:playFade', clampFade(s)); emit('change'); };
const setStopFade = (s: number) => { emit('update:stopFade', clampFade(s)); emit('change'); };
const setCrossFade = (s: number) => { emit('update:crossFade', clampFade(s)); emit('change'); };

// The Stop button's fade is not bounded by the trim: stopping a cue that is
// halfway through a long bed can reasonably take longer than what is left.
const adjustManualStopFade = (delta: number) => {
  emit('update:manualStopFade', Math.max(0, manualStopFade.value + delta));
  emit('change');
};
// Half the loop at most: the blended head and tail must not overlap.
const clampLoopCrossfade = (v: number) => Math.max(0, Math.min(v, trimmedLength.value / 2));
const adjustLoopCrossfade = (delta: number) => {
  emit('update:loopCrossfade', clampLoopCrossfade(loopCrossfade.value + delta));
  emit('change');
};
const setLoopCrossfade = (s: number) => { emit('update:loopCrossfade', clampLoopCrossfade(s)); emit('change'); };
const setManualStopFade = (s: number) => { emit('update:manualStopFade', Math.max(0, s)); emit('change'); };
// Disabled until the cue loops: the description says so, rather than a
// tooltip only a mouse can find.
const loopCrossfadeHelp = computed(() => (isLooping.value
  ? t('properties.loopCrossfadeHint')
  : `${t('properties.loopCrossfadeHint')} ${t('properties.loopCrossfadeNeedsLoop')}`));

// Start Next marker handlers
const handleStartNextEnabledChange = (event: Event) => {
  const enabled = (event.target as HTMLInputElement).checked;
  emit('update:startNextEnabled', enabled);
  // First enable: default the marker near the end of the trimmed region so
  // it's immediately visible and roughly where a segue point usually lives.
  if (enabled && startNextTime.value <= 0) {
    emit('update:startNextTime', Math.max(inPoint.value, outPoint.value - 5));
  }
  emit('change');
};

const handleStartNextFadeOutChange = (event: Event) => {
  emit('update:startNextFadeOut', (event.target as HTMLInputElement).checked);
  emit('change');
};

const adjustStartNextTime = (delta: number) => {
  const newValue = Math.max(inPoint.value, Math.min(startNextTime.value + delta, outPoint.value));
  emit('update:startNextTime', newValue);
  emit('change');
};

const setStartNextTime = (s: number) => {
  emit('update:startNextTime', Math.max(inPoint.value, Math.min(s, outPoint.value)));
  emit('change');
};

// Trim silence from start and end based on waveform peaks
const trimSilence = () => {
  if (!waveformData.value || waveformData.value.length === 0) {
    console.warn('No waveform data available for trimming');
    return;
  }

  // Emit trimSilence event to trigger batch trimming in parent.
  // The parent (handleTrimSilence) trims every selected item INDIVIDUALLY
  // based on its own waveform and persists the result itself. We deliberately
  // do NOT emit 'change' here: that would run the multi-select snapshot-diff
  // in handleSave and overwrite each item's individually-computed in/out
  // points with the anchor item's values.
  emit('trimSilence');
};

// Normalize audio to target loudness
const normalizeAudio = () => {
  if (!waveformData.value || waveformData.value.length === 0) {
    console.warn('No waveform data available for normalization');
    return;
  }

  // Emit normalize event to trigger batch normalization in parent.
  // The parent (handleNormalize) normalizes every selected item INDIVIDUALLY
  // to the target loudness using each item's own intrinsic loudness, and
  // persists itself. We deliberately do NOT emit 'change' here: that would run
  // the multi-select snapshot-diff in handleSave and overwrite each item's
  // individually-computed volume with the anchor item's volume.
  emit('normalize');
};

// Draw waveform on canvas
const drawWaveform = () => {
  const canvas = waveformCanvas.value;
  if (!canvas) return;

  const ctx = canvas.getContext('2d');
  if (!ctx) return;

  // Read once and lay the whole frame out against it. Everything below — the
  // lanes, the grid, the fade diagonals, the playhead arrows — is positioned
  // from this, so a resize landing mid-draw would tear the frame.
  const canvasHeight = containerHeight.value;

  // Set canvas dimensions with device pixel ratio
  const dpr = window.devicePixelRatio || 1;
  canvas.width = canvasWidth.value * dpr;
  canvas.height = canvasHeight * dpr;
  canvas.style.width = `${canvasWidth.value}px`;
  // The displayed height deliberately stays with the stylesheet (height: 100%)
  // rather than being pinned here. Now that the backing store is measured from
  // the same box, the two agree; leaving CSS in charge also means a splitter
  // drag rescales the last bitmap for the frame before the redraw lands,
  // instead of opening a gap under the canvas.
  ctx.scale(dpr, dpr);

  // Clear canvas with background color
  const bgColor = getComputedStyle(document.documentElement).getPropertyValue('--color-background').trim();
  ctx.fillStyle = bgColor || '#000';
  ctx.fillRect(0, 0, canvasWidth.value, canvasHeight);

  // The ruler along the top, as on every timeline tab; everything else is
  // drawn in the box under it.
  drawTimeRuler(ctx, {
    duration: duration.value, start: visibleStart.value, end: visibleEnd.value,
    width: canvasWidth.value, height: canvasHeight,
  });
  const contentHeight = Math.max(1, canvasHeight - CONTENT_TOP);
  const contentView = {
    duration: duration.value, start: visibleStart.value, end: visibleEnd.value,
    width: canvasWidth.value, height: contentHeight,
  };
  ctx.save();
  ctx.translate(0, CONTENT_TOP);

  if (hasWaveform.value && waveformLanes.value.length > 0 && duration.value > 0) {
    // One lane per source channel (#47): a stereo file draws L above R, mono
    // (and legacy single-array waveforms) fills the full height as before.
    // Bars in the output target's zone colours (the stereo meter's palette),
    // so the waveform and meter always agree visually.
    drawWaveformLanes(ctx, waveformLanes.value, contentView, {
      volume: props.audioItem?.volume ?? 1,
      targetDb: outputTargetLevels.value?.autoVolumeTargetDb ?? -23,
      rgbFor: db => hexToRgb(colorForLevel(db)),
      // What you will hear: trim, fades and automation on top of the volume.
      gainAt: makeCompositeGain(props.audioItem),
    });
  } else {
    // Draw "No Waveform Data" message
    ctx.font = '14px sans-serif';
    ctx.fillStyle = 'rgba(128, 128, 128, 0.5)';
    ctx.textAlign = 'center';
    ctx.fillText(t('properties.noWaveformData'), canvasWidth.value / 2, contentHeight / 2);
    ctx.textAlign = 'left';
  }

  // Draw center line
  ctx.strokeStyle = 'rgba(128, 128, 128, 0.3)';
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(0, contentHeight / 2);
  ctx.lineTo(canvasWidth.value, contentHeight / 2);
  ctx.stroke();

  // Draw fade visualizations (always, regardless of playback state)
  drawPlaybackMarkers(ctx, {
    inPoint: inPoint.value,
    outPoint: outPoint.value,
    playFade: props.audioItem.playFade,
    stopFade: props.audioItem.stopFade,
    crossFade: props.audioItem.crossFade,
    startNextEnabled: props.audioItem.startNextEnabled,
    startNextTime: startNextTime.value,
    startNextFadeOut: props.audioItem.startNextFadeOut,
    fadeOutDuration: props.audioItem.fadeOutDuration,
  }, contentView);
  ctx.restore();

  // Selection, edit cursor and the automation envelope (full-height
  // coordinates: yAtDb already keeps clear of the ruler).
  if (duration.value > 0) drawAutomationLayer(ctx, canvasHeight);

  // Draw playhead if item is currently playing
  if (playbackPosition.value !== null && duration.value > 0) {
    const relativeTime = playbackPosition.value - visibleStart.value;
    const playheadX = (relativeTime / visibleDuration.value) * canvasWidth.value;
    if (playheadX >= 0 && playheadX <= canvasWidth.value) {
      const itemColor = props.audioItem.color || 'var(--color-accent)';
      ctx.strokeStyle = itemColor;
      ctx.lineWidth = 2;
      ctx.beginPath();
      ctx.moveTo(playheadX, 0);
      ctx.lineTo(playheadX, canvasHeight);
      ctx.stroke();
      ctx.fillStyle = itemColor;
      ctx.beginPath();
      ctx.moveTo(playheadX, 10);
      ctx.lineTo(playheadX - 6, 0);
      ctx.lineTo(playheadX + 6, 0);
      ctx.closePath();
      ctx.fill();
      ctx.beginPath();
      ctx.moveTo(playheadX, canvasHeight - 10);
      ctx.lineTo(playheadX - 6, canvasHeight);
      ctx.lineTo(playheadX + 6, canvasHeight);
      ctx.closePath();
      ctx.fill();
    }
  }
};

// Throttle drawWaveform to prevent excessive redraws
let drawTimeout: NodeJS.Timeout | null = null;
const throttledDraw = () => {
  if (drawTimeout) clearTimeout(drawTimeout);
  drawTimeout = setTimeout(() => {
    drawWaveform();
  }, 16); // ~60fps
};

// Watch for changes and redraw
watch([
  () => view.zoom,
  () => view.viewStart,
  () => props.audioItem?.volume,
  () => props.audioItem?.inPoint,
  () => props.audioItem?.outPoint,
  () => props.audioItem?.playFade,
  () => props.audioItem?.stopFade,
  () => props.audioItem?.crossFade,
  () => props.audioItem?.startNextEnabled,
  () => props.audioItem?.startNextTime,
  () => props.audioItem?.startNextFadeOut,
  () => props.audioItem?.fadeOutDuration,
  () => props.audioItem?.volumeAutomation,
  () => props.audioItem?.volumeAutomation?.points,
  () => props.audioItem?.volumeAutomation?.enabled,
  cursorTime,
  selection,
  selectedKeyframe,
  waveformData,
  playbackPosition,
  () => outputTargetLevels.value?.autoVolumeTargetDb,
], () => {
  throttledDraw();
});

// Redraw whenever waveform data arrives or changes. `immediate` ensures we
// schedule a draw on mount even if the data was already set (e.g. when the
// panel is reopened for the same item). The canvas ref is null at that point
// so drawWaveform returns early; the real draw comes from onMounted's rAF.
watch(() => props.audioItem?.waveform, () => {
  throttledDraw();
}, { immediate: true });

// Leaving multi-selection re-creates the canvas element (it's only rendered in
// single-item mode). Re-measure the container and draw once it's back, and run
// the self-heal that was suppressed while multiple items were selected.
watch(() => props.multiSelect, (multi) => {
  if (multi) return;
  nextTick(() => requestAnimationFrame(() => {
    measureContainer();
    drawWaveform();
    ensureWaveform();
  }));
});

// Watch for canvas size changes. clientWidth/clientHeight are the container's
// CONTENT box — its 1px border is excluded — which is exactly the area the
// canvas element fills, so the backing store matches it 1:1.
const measureContainer = () => {
  const el = waveformContainer.value;
  if (!el) return;
  containerWidth.value = el.clientWidth;
  containerHeight.value = el.clientHeight;
};

const resizeObserver = ref<ResizeObserver | null>(null);

onMounted(() => {
  // Wait for layout to settle (nextTick ensures the DOM is painted; rAF
  // ensures the browser has committed the layout pass so clientWidth is real).
  nextTick(() => {
    requestAnimationFrame(() => {
      measureContainer();
      drawWaveform();

      // If waveform data is missing, ask the server to (re)generate it.
      ensureWaveform();
    });
  });

  if (waveformContainer.value) {
    // Fires for height as well as width, so dragging the panel's splitter
    // redraws at the new size instead of stretching the last bitmap.
    resizeObserver.value = new ResizeObserver(() => {
      measureContainer();
      throttledDraw();
    });
    resizeObserver.value.observe(waveformContainer.value);
  }
});

onUnmounted(() => {
  // Clear any pending draw / self-heal operations
  if (drawTimeout) clearTimeout(drawTimeout);
  if (ensureWaveformTimer) clearTimeout(ensureWaveformTimer);

  // Clean up resize observer
  if (resizeObserver.value && waveformContainer.value) {
    resizeObserver.value.unobserve(waveformContainer.value);
    resizeObserver.value.disconnect();
  }
});

</script>

<style scoped>
/* The Playback tab in the shared timeline frame (assets/styles/timeline.scss
   has the toolbar, timeline box, inspector and field styles). What is left
   here is this tab's own: the fader, the canvas, and the handles on it.

   The floor is what fits in the panel at its 300px default (~152px of content
   after the header, tabs and padding), so nothing scrolls at the height the
   panel has always had; below that .properties-content takes over. */
.waveform-trimmer {
  min-height: 150px;
}

/* Volume Control */
.volume-control-section {
  display: flex;
  flex-direction: column;
  gap: var(--spacing-xs);
  min-width: 70px;
  /* Never squeezed: its scale would spill over the toolbar beside it. */
  flex: 0 0 auto;
}

.volume-label {
  display: flex;
  flex-direction: column;
  gap: 2px;
  font-size: 11px;
  color: var(--color-text-secondary);
}

.db-entry {
  display: flex;
  align-items: baseline;
  gap: 3px;
}

.db-input {
  width: 52px;
  padding: 2px 4px;
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius-sm);
  color: var(--color-text-primary);
  font-size: 13px;
  font-weight: 600;
  font-variant-numeric: tabular-nums;
  text-align: right;
}

.db-input:focus {
  outline: none;
  border-color: var(--color-accent);
}

.db-unit {
  font-size: 11px;
}

.volume-slider-container {
  display: flex;
  gap: var(--spacing-xs);
  align-items: center;
  flex: 1;
  min-height: 0;
}

.volume-slider-vertical {
  writing-mode: vertical-lr;
  direction: rtl;
  width: 150px;
  height: 100%;
  cursor: pointer;
  -webkit-appearance: slider-vertical;
  appearance: slider-vertical;
  background: transparent;
  border-radius: 4px;
  position: relative;
}

/* Volume slider track with dB steps */
.volume-slider-vertical::-webkit-slider-runnable-track {
  width: 20px;
  height: 100%;
  background: linear-gradient(to top,
    #1a4d2e 0%,      /* Darker green under -36db */
    #1a4d2e 33%,     /* -36db */
    #22c55e 33%,     /* Dark green -36 to -18 */
    #22c55e 50%,     /* -18db */
    #16a34a 50%,     /* Green -18 to -6 */
    #16a34a 75%,     /* -6db */
    #eab308 75%,     /* Yellow -6 to -1 */
    #eab308 93%,     /* -1db */
    #dc2626 93%,     /* Red -1 to 0 */
    #dc2626 97%,     /* 0db */
    #991b1b 97%,     /* Dark red above 0 */
    #991b1b 100%
  );
  border-radius: 4px;
  /* Add horizontal lines for dB steps */
  background-image:
    repeating-linear-gradient(0deg,
      rgba(0, 0, 0, 0.3) 0px,
      rgba(0, 0, 0, 0.3) 1px,
      transparent 1px,
      transparent 10%
    );
}

.volume-slider-vertical::-moz-range-track {
  width: 20px;
  height: 100%;
  background: linear-gradient(to top,
    #1a4d2e 0%,
    #1a4d2e 33%,
    #22c55e 33%,
    #22c55e 50%,
    #16a34a 50%,
    #16a34a 75%,
    #eab308 75%,
    #eab308 93%,
    #dc2626 93%,
    #dc2626 97%,
    #991b1b 97%,
    #991b1b 100%
  );
  border-radius: 4px;
  background-image:
    repeating-linear-gradient(0deg,
      rgba(0, 0, 0, 0.3) 0px,
      rgba(0, 0, 0, 0.3) 1px,
      transparent 1px,
      transparent 10%
    );
}

.volume-slider-vertical::-webkit-slider-thumb {
  -webkit-appearance: none;
  appearance: none;
  width: 24px;
  height: 14px;
  background: var(--volume-handle-color, var(--color-text-primary));
  cursor: pointer;
  border-radius: 3px;
  box-shadow: 0 2px 4px rgba(0, 0, 0, 0.4);
  border: 2px solid rgba(255, 255, 255, 0.3);
}

.volume-slider-vertical::-moz-range-thumb {
  width: 24px;
  height: 14px;
  background: var(--volume-handle-color, var(--color-text-primary));
  cursor: pointer;
  border-radius: 3px;
  border: 2px solid rgba(255, 255, 255, 0.3);
  box-shadow: 0 2px 4px rgba(0, 0, 0, 0.4);
}

.volume-markers {
  display: flex;
  flex-direction: column;
  justify-content: space-between;
  height: 100%;
  font-size: 9px;
  color: var(--color-text-secondary);
}


/* The automation switch, its icon and its description read as one control. */
.automation-toggle {
  gap: 6px;
  user-select: none;

  .material-symbols-rounded { font-size: 18px; color: var(--color-text-secondary); }
}

/* Start Next's switch, in its time's label, in the marker's own green. */
.start-next-toggle {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  min-width: 0;
  color: var(--color-text-primary);
  cursor: pointer;

  input[type='checkbox'] {
    width: 14px;
    height: 14px;
    flex: 0 0 auto;
    margin: 0;
    padding: 0;
    accent-color: rgb(22, 163, 74);
    cursor: pointer;
  }
}

.volume-label-text {
  display: flex;
  align-items: center;
  gap: 2px;
}

.waveform-canvas {
  display: block;
  width: 100%;
  height: 100%;
}

/* Multi-selection placeholder shown in place of the single-item canvas.
   Fills the (already-bordered) waveform container. */
.waveform-multi-message {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: var(--spacing-xs);
  width: 100%;
  height: 100%;
  color: var(--color-text-secondary);
  text-align: center;
  padding: 0 var(--spacing-lg);
}

.waveform-multi-message .material-symbols-rounded {
  font-size: 28px;
  opacity: 0.7;
}

.waveform-multi-message p {
  font-size: 12px;
  max-width: 320px;
}

/* Trim Handles */
.trim-handle {
  position: absolute;
  top: 0;
  bottom: 0;
  width: 2px;
  cursor: ew-resize;
  z-index: 10;
  user-select: none;
}

.trim-handle-in {
  background: rgba(34, 197, 94, 0.3);
}

.trim-handle-out {
  background: rgba(239, 68, 68, 0.3);
}

.trim-line {
  position: absolute;
  top: 0;
  bottom: 0;
  width: 2px;
  background: currentColor;
}

.trim-handle-in .trim-line {
  background: rgb(34, 197, 94);
}

.trim-handle-out .trim-line {
  background: rgb(239, 68, 68);
}

.trim-grip {
  position: absolute;
  /* The middle of the waveform, which sits under the ruler band. */
  top: calc(50% + 9px);
  transform: translateY(-50%);
  width: 20px;
  height: 32px;
  background: var(--color-surface);
  border: 2px solid currentColor;
  border-radius: var(--border-radius-sm);
  display: flex;
  align-items: center;
  justify-content: center;
  box-shadow: 0 2px 4px rgba(0, 0, 0, 0.2);
  user-select: none;
}

.trim-handle-in .trim-grip {
  left: -10px;
  color: rgb(34, 197, 94);
}

.trim-handle-out .trim-grip {
  right: -10px;
  color: rgb(239, 68, 68);
}

.trim-grip .material-symbols-rounded {
  font-size: 14px;
}

/* Fade Handles */
.fade-handle {
  position: absolute;
  top: 0;
  bottom: 0;
  width: 2px;
  cursor: ew-resize;
  z-index: 11;
  user-select: none;
}

.fade-line {
  position: absolute;
  top: 0;
  bottom: 0;
  width: 2px;
}

.fade-line-red {
  background: rgba(220, 38, 38, 0.8);
}

.fade-line-yellow {
  background: rgba(234, 179, 8, 0.8);
}

.fade-line-green {
  background: rgba(22, 163, 74, 0.8);
}

.fade-grip {
  position: absolute;
  /* The middle of the waveform, which sits under the ruler band. */
  top: calc(50% + 9px);
  transform: translateY(-50%);
  width: 24px;
  height: 24px;
  background: var(--color-surface);
  border: 2px solid currentColor;
  border-radius: 50%;
  display: flex;
  align-items: center;
  justify-content: center;
  box-shadow: 0 2px 4px rgba(0, 0, 0, 0.3);
  user-select: none;
  transition: transform 0.15s ease;
}

.fade-handle:hover .fade-grip {
  transform: translateY(-50%) scale(1.15);
}

.fade-grip-red {
  color: rgb(220, 38, 38);
  border-color: rgb(220, 38, 38);
}

.fade-grip-yellow {
  color: rgb(234, 179, 8);
  border-color: rgb(234, 179, 8);
}

.fade-grip-green {
  color: rgb(22, 163, 74);
  border-color: rgb(22, 163, 74);
}

.fade-handle-play .fade-grip {
  left: 50%;
  transform: translate(-50%, -50%);
}

.fade-handle-play:hover .fade-grip {
  transform: translate(-50%, -50%) scale(1.15);
}

.fade-handle-stop .fade-grip,
.fade-handle-cross .fade-grip,
.fade-handle-startnext .fade-grip {
  left: 50%;
  transform: translate(-50%, -50%);
}

.fade-handle-stop:hover .fade-grip,
.fade-handle-cross:hover .fade-grip,
.fade-handle-startnext:hover .fade-grip {
  transform: translate(-50%, -50%) scale(1.15);
}

.fade-grip .material-symbols-rounded {
  font-size: 14px;
}

/* Trim Overlays */
.trim-overlay {
  position: absolute;
  top: 0;
  bottom: 0;
  background: rgba(0, 0, 0, 0.5);
  pointer-events: none;
}

.trim-overlay-left {
  left: 0;
}

.trim-overlay-right {
  right: 0;
  width: auto;
  left: auto;
}

</style>

<template>
  <!-- Note: NOT inside <Teleport> — Vue scoped styles don't reach teleported
       nodes, which would leave the modal unstyled in production builds. -->
  <div v-if="open" class="project-settings-backdrop" @click.self="close">
    <div class="project-settings-modal">
        <header class="modal-header">
          <h2>{{ t('settings.title') }}</h2>
          <button class="close-x" @click="close">✕</button>
        </header>

        <!-- Tab Navigation (styled to match the Properties panel) -->
        <div class="settings-tabs">
          <button
            v-for="tab in tabs"
            :key="tab.id"
            :class="['tab-btn', { active: activeTab === tab.id }]"
            @click="activeTab = tab.id"
          >
            <span class="material-symbols-rounded">{{ tab.icon }}</span>
            <span>{{ tab.label }}</span>
          </button>
        </div>

        <div class="modal-body">
          <!-- ================= Audio Routing ================= -->
          <template v-if="activeTab === 'audio'">
            <!-- Audio device (default for cue playback) -->
            <section class="settings-field">
              <label class="settings-label">
                <span class="material-symbols-rounded">speaker</span>
                {{ t('settings.audioDevice') }}
              </label>
              <select
                class="settings-select"
                :value="audioDeviceId"
                @change="onAudioDeviceChange"
              >
                <option :value="''">{{ t('settings.noneSelected') }}</option>
                <option v-for="d in devices" :key="d.id" :value="d.id">
                  {{ d.display_name }}{{ d.is_default ? ' (' + t('common.default') + ')' : '' }}
                </option>
              </select>
              <p class="settings-help">{{ t('settings.audioDeviceHelp') }}</p>
            </section>

            <!-- Preview device (used by headphones button) -->
            <section class="settings-field">
              <label class="settings-label">
                <span class="material-symbols-rounded">headphones</span>
                {{ t('settings.previewDevice') }}
              </label>
              <select
                class="settings-select"
                :value="previewDeviceId"
                @change="onPreviewDeviceChange"
              >
                <option :value="''">{{ t('settings.noneSelected') }}</option>
                <option v-for="d in devices" :key="d.id" :value="d.id">
                  {{ d.display_name }}{{ d.is_default ? ' (' + t('common.default') + ')' : '' }}
                </option>
              </select>
              <p class="settings-help">{{ t('settings.previewDeviceHelp') }}</p>
            </section>

            <!-- LTC device (timecode output) -->
            <section class="settings-field">
              <label class="settings-label">
                <span class="material-symbols-rounded">schedule</span>
                {{ t('settings.ltcDevice') }}
              </label>
              <select
                class="settings-select"
                :value="ltcDeviceId"
                @change="onLtcDeviceChange"
              >
                <option :value="''">{{ t('settings.noneSelected') }}</option>
                <option v-for="d in devices" :key="d.id" :value="d.id">
                  {{ d.display_name }}{{ d.is_default ? ' (' + t('common.default') + ')' : '' }}
                </option>
              </select>
              <p class="settings-help">{{ t('settings.ltcDeviceHelp') }}</p>
            </section>

            <!-- Output Target (loudness platform standard) -->
            <section class="settings-field">
              <label class="settings-label">
                <span class="material-symbols-rounded">tune</span>
                {{ t('settings.outputTarget') }}
              </label>
              <select
                class="settings-select"
                :value="outputTarget"
                @change="onOutputTargetChange"
              >
                <option value="ebu-r128">{{ t('settings.outputTargetEbuR128') }}</option>
                <option value="streaming">{{ t('settings.outputTargetStreaming') }}</option>
                <option value="radio">{{ t('settings.outputTargetRadio') }}</option>
                <option value="netflix">{{ t('settings.outputTargetNetflix') }}</option>
                <option value="live">{{ t('settings.outputTargetLive') }}</option>
              </select>
              <p class="settings-help">{{ t('settings.outputTargetHelp') }}</p>
            </section>
          </template>

          <!-- ================= Playback Behaviour ================= -->
          <template v-else-if="activeTab === 'playback'">
            <!-- Default transition mode -->
            <section class="settings-field">
              <label class="settings-label">
                <span class="material-symbols-rounded">swap_horiz</span>
                {{ t('settings.transitionMode') }}
              </label>
              <select
                class="settings-select"
                :value="defaultTransitionMode"
                @change="onDefaultTransitionModeChange"
              >
                <option value="crossfade">{{ t('settings.transitionModeCrossfade') }}</option>
                <option value="start-next">{{ t('settings.transitionModeStartNext') }}</option>
              </select>
              <p class="settings-help">{{ t('settings.transitionModeHelp') }}</p>
            </section>

            <!-- Auto-cue next item without end behaviour (#28) -->
            <section class="settings-field">
              <label class="settings-label settings-label--checkbox">
                <input
                  type="checkbox"
                  :checked="autoCueNextWithoutEndBehavior"
                  @change="onAutoCueNextChange"
                />
                {{ t('settings.autoCueNext') }}
              </label>
              <p class="settings-help">{{ t('settings.autoCueNextHelp') }}</p>
            </section>

            <!-- Project-wide Stop All fade-out time -->
            <section class="settings-field">
              <label class="settings-label">
                <span class="material-symbols-rounded">stop_circle</span>
                {{ t('settings.stopAllFade') }}
              </label>
              <input
                type="number"
                class="settings-input"
                min="0"
                step="0.1"
                :value="stopAllFadeSeconds"
                @change="onStopAllFadeChange"
              />
              <p class="settings-help">{{ t('settings.stopAllFadeHelp') }}</p>
            </section>

            <!-- Auto volume and trim -->
            <section class="settings-field">
              <label class="settings-label settings-label--checkbox">
                <input
                  type="checkbox"
                  :checked="disableAutoVolumeAndTrim"
                  @change="onDisableAutoVolumeAndTrimChange"
                />
                {{ t('settings.disableAutoVolumeAndTrim') }}
              </label>
            </section>

            <!-- Disable brickwall limiter -->
            <section class="settings-field">
              <label class="settings-label settings-label--checkbox">
                <input
                  type="checkbox"
                  :checked="disableLimiter"
                  @change="onDisableLimiterChange"
                />
                {{ t('settings.disableLimiter') }}
              </label>
              <p class="settings-help">{{ t('settings.disableLimiterHelp') }}</p>
            </section>

            <!-- Disable silence warning -->
            <section class="settings-field">
              <label class="settings-label settings-label--checkbox">
                <input
                  type="checkbox"
                  :checked="disableSilenceWarning"
                  @change="onDisableSilenceWarningChange"
                />
                {{ t('settings.disableSilenceWarning') }}
              </label>
              <p class="settings-help">{{ t('settings.disableSilenceWarningHelp') }}</p>
            </section>
          </template>

          <!-- ================= User Interface ================= -->
          <template v-else-if="activeTab === 'ui'">
            <!-- Meter Display Mode -->
            <section class="settings-field">
              <label class="settings-label">
                <span class="material-symbols-rounded">bar_chart</span>
                {{ t('settings.meterMode') }}
              </label>
              <select
                class="settings-select"
                :value="meterMode"
                @change="onMeterModeChange"
              >
                <option value="LUFS">{{ t('settings.meterModeLufs') }}</option>
                <option value="dBFS">{{ t('settings.meterModeDbfs') }}</option>
                <option value="dBTP">{{ t('settings.meterModeDbtp') }}</option>
                <option value="RMS">{{ t('settings.meterModeRms') }}</option>
              </select>
              <p class="settings-help">{{ t('settings.meterModeHelp') }}</p>
            </section>

            <!-- Keep the currently-playing item centred in the list -->
            <section class="settings-field">
              <label class="settings-label settings-label--checkbox">
                <input
                  type="checkbox"
                  :checked="scrollToPlaying"
                  @change="onScrollToPlayingChange"
                />
                {{ t('settings.scrollToPlaying') }}
              </label>
              <p class="settings-help">{{ t('settings.scrollToPlayingHelp') }}</p>
            </section>
          </template>
        </div>

      <footer class="modal-footer">
        <button class="modal-btn" @click="close">{{ t('settings.close') }}</button>
      </footer>
    </div>
  </div>
</template>

<script setup lang="ts">
import { useOutputTarget } from '~/composables/useOutputTarget';

const props = defineProps<{ open: boolean }>();
const emit  = defineEmits<{ (e: 'close'): void }>();

const { t } = useLocalization();
const server = useLiveplayServer();
const { currentProject } = useProject();

const devices = computed(() => server.devices ?? []);

// Tabs mirror the Properties panel's tab styling. Grouping:
//  - audio    : device routing + loudness target
//  - playback : transitions, auto-cue, stop-all fade, processing toggles
//  - ui       : metering + list behaviour
const activeTab = ref<'audio' | 'playback' | 'ui'>('audio');
const tabs = computed(() => [
  { id: 'audio'    as const, icon: 'graphic_eq', label: t('settings.tabAudioRouting') },
  { id: 'playback' as const, icon: 'play_circle', label: t('settings.tabPlaybackBehaviour') },
  { id: 'ui'       as const, icon: 'desktop_windows', label: t('settings.tabUserInterface') },
]);

// The settings live on the project document; we read them from there and
// patch via the server endpoint.
const audioDeviceId          = computed(() => (currentProject.value as any)?.settings?.defaultOutputDevice || '');
const previewDeviceId        = computed(() => (currentProject.value as any)?.settings?.previewDevice || '');
const ltcDeviceId            = computed(() => (currentProject.value as any)?.settings?.ltcDevice || '');
const outputTarget           = computed(() => (currentProject.value as any)?.settings?.outputTarget || 'ebu-r128');
const disableAutoVolumeAndTrim = computed(() => !!(currentProject.value as any)?.settings?.disableAutoVolumeAndTrim);
const disableLimiter           = computed(() => !!(currentProject.value as any)?.settings?.disableLimiter);
const disableSilenceWarning    = computed(() => !!(currentProject.value as any)?.settings?.disableSilenceWarning);
const defaultTransitionMode    = computed(() => (currentProject.value as any)?.settings?.defaultTransitionMode || 'crossfade');
// Defaults ON (undefined → true) so legacy projects and new projects both
// arm the next item as "Up Next" for cues without an end behaviour. (#28)
const autoCueNextWithoutEndBehavior = computed(() => (currentProject.value as any)?.settings?.autoCueNextWithoutEndBehavior !== false);
// Project-wide Stop All fade, stored in ms (default 1000). Shown in seconds.
const stopAllFadeSeconds = computed(() => {
  const ms = (currentProject.value as any)?.settings?.stopAllFadeMs;
  return ((typeof ms === 'number' ? ms : 1000) / 1000);
});
// UI scrolls to keep the currently-playing item centred (default OFF).
const scrollToPlaying = computed(() => !!(currentProject.value as any)?.settings?.uiScrollToPlaying);
const { meterMode: currentMeterMode } = useOutputTarget();
const meterMode              = computed(() => (currentProject.value as any)?.settings?.meterMode || currentMeterMode.value);

// Make sure devices are loaded when the modal opens.
watch(() => props.open, async (v) => {
  if (v) await server.fetchDevices();
});

// Refresh devices on first mount too.
onMounted(async () => {
  try { await server.fetchDevices(); } catch { /* connection may not be ready yet */ }
});

async function applyPatch(patch: Record<string, any>) {
  // Optimistic local update so the UI reflects the change immediately.
  if (currentProject.value) {
    const settings = ((currentProject.value as any).settings ?? {});
    (currentProject.value as any).settings = { ...settings, ...patch };
  }
  try {
    await server.patchSettings(patch);
  } catch (e) {
    console.warn('[ProjectSettings] patch failed:', e);
  }
}

function onAudioDeviceChange(e: Event) {
  const v = (e.target as HTMLSelectElement).value;
  applyPatch({ defaultOutputDevice: v || null });
}
function onPreviewDeviceChange(e: Event) {
  const v = (e.target as HTMLSelectElement).value;
  applyPatch({ previewDevice: v || null });
}
function onLtcDeviceChange(e: Event) {
  const v = (e.target as HTMLSelectElement).value;
  applyPatch({ ltcDevice: v || null });
}
function onOutputTargetChange(e: Event) {
  const v = (e.target as HTMLSelectElement).value;
  applyPatch({ outputTarget: v });
}
function onMeterModeChange(e: Event) {
  const v = (e.target as HTMLSelectElement).value;
  applyPatch({ meterMode: v });
}
function onDisableAutoVolumeAndTrimChange(e: Event) {
  applyPatch({ disableAutoVolumeAndTrim: (e.target as HTMLInputElement).checked });
}
function onDisableLimiterChange(e: Event) {
  applyPatch({ disableLimiter: (e.target as HTMLInputElement).checked });
}
function onDisableSilenceWarningChange(e: Event) {
  applyPatch({ disableSilenceWarning: (e.target as HTMLInputElement).checked });
}
function onDefaultTransitionModeChange(e: Event) {
  applyPatch({ defaultTransitionMode: (e.target as HTMLSelectElement).value });
}
function onAutoCueNextChange(e: Event) {
  applyPatch({ autoCueNextWithoutEndBehavior: (e.target as HTMLInputElement).checked });
}
function onStopAllFadeChange(e: Event) {
  const seconds = parseFloat((e.target as HTMLInputElement).value);
  const ms = Number.isFinite(seconds) ? Math.max(0, Math.round(seconds * 1000)) : 1000;
  applyPatch({ stopAllFadeMs: ms });
}
function onScrollToPlayingChange(e: Event) {
  applyPatch({ uiScrollToPlaying: (e.target as HTMLInputElement).checked });
}

function close() {
  emit('close');
}
</script>

<style scoped>
.project-settings-backdrop {
  position: fixed;
  inset: 0;
  background: rgba(0, 0, 0, 0.55);
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: 1000;
}

.project-settings-modal {
  background: var(--color-background);
  border: 1px solid var(--color-border);
  border-radius: 10px;
  width: min(560px, 92vw);
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
.modal-header h2 {
  margin: 0;
  font-size: 18px;
}
.close-x {
  background: none;
  border: none;
  color: var(--color-text-secondary);
  font-size: 18px;
  cursor: pointer;
}
.close-x:hover {
  color: var(--color-text-primary);
}

/* Tab Navigation — matches PropertiesPanel.vue .properties-tabs / .tab-btn */
.settings-tabs {
  display: flex;
  gap: 2px;
  padding: 0 20px;
  border-bottom: 1px solid var(--color-border);
  background-color: var(--color-surface);
  overflow-x: auto;
}
.tab-btn {
  display: flex;
  align-items: center;
  gap: var(--spacing-sm, 8px);
  padding: var(--spacing-sm, 8px) var(--spacing-md, 12px);
  background: none;
  border: none;
  border-bottom: 2px solid transparent;
  cursor: pointer;
  color: var(--color-text-secondary);
  font-size: 13px;
  font-weight: 500;
  white-space: nowrap;
  transition: all 0.2s;
}
.tab-btn .material-symbols-rounded {
  font-size: 18px;
  color: inherit;
}
.tab-btn:hover {
  color: var(--color-text-primary);
  background-color: var(--color-surface-hover);
}
.tab-btn.active {
  color: var(--color-accent);
  border-bottom-color: var(--color-accent);
}

.modal-body {
  padding: 16px 20px;
  display: flex;
  flex-direction: column;
  gap: 18px;
  overflow-y: auto;
}

.settings-field {
  display: flex;
  flex-direction: column;
  gap: 6px;
}
.settings-label {
  display: flex;
  align-items: center;
  gap: 8px;
  font-size: 13px;
  color: var(--color-text-secondary);
  font-weight: 500;
}
.settings-select,
.settings-input {
  width: 100%;
  padding: 10px 12px;
  background: var(--color-surface);
  color: var(--color-text-primary);
  border: 1px solid var(--color-border);
  border-radius: 6px;
  font-size: 14px;
}
.settings-select:focus,
.settings-input:focus {
  outline: none;
  border-color: var(--color-accent);
}
.settings-help {
  margin: 0;
  font-size: 12px;
  color: var(--color-text-secondary);
}
.settings-label--checkbox {
  flex-direction: row;
  gap: 10px;
  font-size: 14px;
  color: var(--color-text-primary);
  cursor: pointer;
}
.settings-label--checkbox input[type="checkbox"] {
  width: 16px;
  height: 16px;
  cursor: pointer;
  accent-color: var(--color-accent);
}

.modal-footer {
  padding: 12px 20px;
  border-top: 1px solid var(--color-border);
  display: flex;
  justify-content: flex-end;
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
.modal-btn:hover {
  background: var(--color-surface-hover);
}
</style>

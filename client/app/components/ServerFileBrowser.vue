<template>
  <div class="server-file-browser">
    <div class="server-file-browser__bar">
      <button class="btn" :disabled="!canGoUp" @click="goUp" title="Up one level">
        <span class="material-symbols-rounded" style="font-size:16px;vertical-align:middle;">arrow_upward</span>
        Up
      </button>
      <input
        v-model="pathInput"
        class="path-input"
        @keydown.enter="goTo(pathInput)"
        placeholder="Server path (Enter to browse)"
      />
      <button class="btn primary" @click="goTo(pathInput)">Go</button>
    </div>

    <div v-if="error" class="error">{{ error }}</div>
    <div v-else-if="loading" class="status">Loading…</div>

    <ul v-else class="entries">
      <li v-for="(entry, idx) in sortedEntries"
          :key="entry.full_path"
          class="entry"
          :class="[entry.kind, { selected: isSelected(entry.full_path) }]"
          @click="onEntryClick(entry, idx, $event)"
          @dblclick="onEntryActivate(entry)">
        <span class="icon material-symbols-rounded">{{ iconFor(entry) }}</span>
        <span class="name">{{ entry.name }}</span>
        <span v-if="entry.kind === 'file' && entry.size != null"
              class="size">{{ formatBytes(entry.size) }}</span>
      </li>
      <li v-if="(listing?.entries?.length ?? 0) === 0" class="empty">
        (no audio files or subdirectories)
      </li>
    </ul>

    <div v-if="canSelect" class="server-file-browser__footer">
      <span class="sel-count">{{ selectedCountLabel }}</span>
      <button class="btn primary" :disabled="selected.length === 0" @click="importSelected">
        {{ t('importAudio.importSelected') }}<span v-if="selected.length"> ({{ selected.length }})</span>
      </button>
    </div>
  </div>
</template>

<!--
  ServerFileBrowser.vue
  -----------------------------------------------------------------------
  Browse the *server's* filesystem (not the client's) via /api/fs/list, so
  the client can pick cue files when running against a remote LivePlay
  server. The 1.x client used Electron's dialog.showOpenDialog which only
  worked when client and audio engine ran on the same machine.

  Selection model:
    Files can be multi-selected (plain click = single, Ctrl/Cmd-click =
    toggle, Shift-click = range). A single "Import selected" button at the
    bottom emits the whole batch. Double-clicking a file imports it directly;
    double-clicking a folder/drive descends into it.

  Emits:
    select(fullPaths: string[]) — user picked one or more audio files.
-->
<script setup lang="ts">
import { computed, onMounted, ref, watch } from 'vue';
import { useLiveplayServer } from '~/composables/useLiveplayServer';
import type { ServerFsEntry, ServerFsListing } from '~/types/server';

const props = withDefaults(defineProps<{
  startPath?: string;
  canSelect?: boolean;
}>(), {
  startPath: '',
  canSelect: true,
});

const emit = defineEmits<{
  (e: 'select', fullPaths: string[]): void;
}>();

const server = useLiveplayServer();
const { t }  = useLocalization();
const listing = ref<ServerFsListing | null>(null);
const loading = ref(false);
const error   = ref<string | null>(null);
const pathInput = ref<string>(props.startPath);

// Multi-selection of file paths. `anchorIndex` is the last plain/Ctrl click,
// used as the pivot for Shift-range selection.
const selected   = ref<string[]>([]);
let   anchorIndex = -1;

const canGoUp = computed(() =>
  !!listing.value && (!!listing.value.parent || !listing.value.is_root));

const sortedEntries = computed(() => {
  const entries = listing.value?.entries ?? [];
  const rank: Record<string, number> = { home: 0, drive: 1, dir: 2, file: 3 };
  return [...entries].sort((a, b) => {
    const r = (rank[a.kind] ?? 9) - (rank[b.kind] ?? 9);
    return r !== 0 ? r : a.name.localeCompare(b.name, undefined, { sensitivity: 'base' });
  });
});

const selectedCountLabel = computed(() =>
  selected.value.length ? t('importAudio.selectedCount', { count: selected.value.length }) : '');

function isSelected(p: string): boolean { return selected.value.includes(p); }

function iconFor(entry: ServerFsEntry): string {
  if (entry.kind === 'home')  return 'home';
  if (entry.kind === 'drive') return 'storage';
  if (entry.kind === 'dir')   return 'folder';
  return 'audio_file';
}

async function goTo(path: string) {
  loading.value = true;
  error.value   = null;
  try {
    listing.value   = await server.listServerPath(path);
    pathInput.value = listing.value.path;
    selected.value  = [];
    anchorIndex     = -1;
  } catch (e: any) {
    error.value = String(e.message || e);
  } finally {
    loading.value = false;
  }
}

function goUp() {
  if (!listing.value) return;
  if (listing.value.parent) goTo(listing.value.parent);
  else if (!listing.value.is_root) goTo('');   // at a drive root → drive menu
}

function onEntryClick(entry: ServerFsEntry, index: number, e: MouseEvent) {
  if (!props.canSelect || entry.kind !== 'file') return;
  const multi = e.ctrlKey || e.metaKey;
  const range = e.shiftKey;

  if (range && anchorIndex >= 0) {
    const [lo, hi] = anchorIndex < index ? [anchorIndex, index] : [index, anchorIndex];
    const inRange = sortedEntries.value
      .slice(lo, hi + 1)
      .filter(en => en.kind === 'file')
      .map(en => en.full_path);
    selected.value = multi
      ? Array.from(new Set([...selected.value, ...inRange]))
      : inRange;
  } else if (multi) {
    selected.value = isSelected(entry.full_path)
      ? selected.value.filter(p => p !== entry.full_path)
      : [...selected.value, entry.full_path];
    anchorIndex = index;
  } else {
    selected.value = [entry.full_path];
    anchorIndex = index;
  }
}

function onEntryActivate(entry: ServerFsEntry) {
  if (entry.kind === 'dir' || entry.kind === 'drive' || entry.kind === 'home') goTo(entry.full_path);
  else if (props.canSelect) emit('select', [entry.full_path]);
}

function importSelected() {
  if (selected.value.length === 0) return;
  emit('select', [...selected.value]);
}

function formatBytes(n: number): string {
  if (n < 1024) return `${n} B`;
  const units = ['KB', 'MB', 'GB', 'TB'];
  let v = n / 1024;
  let i = 0;
  while (v >= 1024 && i < units.length - 1) { v /= 1024; i++; }
  return `${v.toFixed(1)} ${units[i]}`;
}

onMounted(() => goTo(props.startPath));
watch(() => props.startPath, p => goTo(p));
</script>

<style lang="scss" scoped>
.server-file-browser {
  display: flex;
  flex-direction: column;
  gap: 8px;

  &__bar {
    display: flex;
    gap: 6px;

    .path-input {
      flex: 1;
      padding: 6px 10px;
      background: #1d1d1d;
      border: 1px solid #333;
      border-radius: 4px;
      color: #eee;
      font-family: var(--font-mono);
    }
  }

  &__footer {
    display: flex;
    align-items: center;
    justify-content: flex-end;
    gap: 12px;

    .sel-count { color: #aaa; font-size: 12px; }
  }

  .btn {
    background: #2a2a2a;
    border: 1px solid #3a3a3a;
    border-radius: 4px;
    padding: 6px 12px;
    color: #ddd;
    cursor: pointer;
    display: inline-flex; align-items: center; gap: 4px;
    &:hover:not(:disabled) { background: #353535; }
    &:disabled { opacity: 0.5; cursor: not-allowed; }
    &.primary  { background: var(--color-accent); border-color: var(--color-accent); color: #fff; }
    &.small    { padding: 2px 8px; font-size: 12px; }
  }

  .entries {
    list-style: none;
    margin: 0;
    padding: 0;
    max-height: 400px;
    overflow: auto;
    border: 1px solid #2a2a2a;
    border-radius: 4px;
    background: #161616;

    .entry {
      display: grid;
      grid-template-columns: 28px 1fr auto;
      align-items: center;
      gap: 8px;
      padding: 6px 10px;
      cursor: pointer;
      border-bottom: 1px solid #222;
      user-select: none;
      &:last-child { border-bottom: none; }
      &:hover { background: #202020; }
      .size   { color: #888; font-size: 11px; font-family: var(--font-mono); }
      // Names stay white for legibility; drives & home are emphasised.
      .name   { color: #ffffff; }
      &.drive .name,
      &.home .name { font-weight: 600; }
      // Drives & folders: white icons. Selectable files: accent icon.
      .icon { font-size: 18px; text-align: center; color: #ffffff; }
      &.file .icon { color: var(--color-accent); }
      // Selected file rows: accent fill, white glyphs for contrast.
      &.selected {
        background: var(--color-accent);
        .name, .icon, .size { color: #fff; }
      }
    }
    .empty {
      padding: 12px;
      text-align: center;
      color: #888;
      font-style: italic;
    }
  }
  .error  { color: #ff7070; padding: 8px; }
  .status { color: #888;   padding: 8px; }
}
</style>

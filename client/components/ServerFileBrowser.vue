<template>
  <div class="server-file-browser">
    <div class="server-file-browser__bar">
      <button class="btn" :disabled="!parentPath" @click="goTo(parentPath)" title="Up one level">
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
      <li v-for="entry in sortedEntries"
          :key="entry.full_path"
          class="entry"
          :class="entry.kind"
          @dblclick="onEntryActivate(entry)">
        <span class="icon material-symbols-rounded">{{ iconFor(entry) }}</span>
        <span class="name">{{ entry.name }}</span>
        <span v-if="entry.kind === 'file' && entry.size != null"
              class="size">{{ formatBytes(entry.size) }}</span>
        <button v-if="entry.kind === 'file' && canSelect"
                class="btn small primary"
                @click="emit('select', entry.full_path)">Add</button>
      </li>
      <li v-if="(listing?.entries?.length ?? 0) === 0" class="empty">
        (no audio files or subdirectories)
      </li>
    </ul>
  </div>
</template>

<!--
  ServerFileBrowser.vue
  -----------------------------------------------------------------------
  Browse the *server's* filesystem (not the client's) via /api/fs/list, so
  the client can pick cue files when running against a remote LivePlay
  server. The 1.x client used Electron's dialog.showOpenDialog which only
  worked when client and audio engine ran on the same machine.

  Emits:
    select(fullPath: string) — user picked an audio file to add as a cue.
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
  (e: 'select', fullPath: string): void;
}>();

const server = useLiveplayServer();
const listing = ref<ServerFsListing | null>(null);
const loading = ref(false);
const error   = ref<string | null>(null);
const pathInput = ref<string>(props.startPath);

const parentPath = computed(() => listing.value?.parent ?? '');

const sortedEntries = computed(() => {
  const entries = listing.value?.entries ?? [];
  const rank: Record<string, number> = { drive: 0, dir: 1, file: 2 };
  return [...entries].sort((a, b) => {
    const r = (rank[a.kind] ?? 9) - (rank[b.kind] ?? 9);
    return r !== 0 ? r : a.name.localeCompare(b.name, undefined, { sensitivity: 'base' });
  });
});

function iconFor(entry: ServerFsEntry): string {
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
  } catch (e: any) {
    error.value = String(e.message || e);
  } finally {
    loading.value = false;
  }
}

function onEntryActivate(entry: ServerFsEntry) {
  if (entry.kind === 'dir' || entry.kind === 'drive') goTo(entry.full_path);
  else if (props.canSelect) emit('select', entry.full_path);
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
      font-family: monospace;
    }
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
      grid-template-columns: 28px 1fr auto auto;
      align-items: center;
      gap: 8px;
      padding: 6px 10px;
      cursor: pointer;
      border-bottom: 1px solid #222;
      &:last-child { border-bottom: none; }
      &:hover { background: #202020; }
      .size   { color: #888; font-size: 11px; font-family: monospace; }
      .name   { color: #eee; }
      &.dir   .name   { color: #ffffff; }
      &.drive .name   { color: #ffffff; font-weight: 600; }
      .icon { font-size: 18px; text-align: center; color: #aaa; }
      &.dir   .icon   { color: var(--color-accent); }
      &.drive .icon   { color: var(--color-accent); }
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

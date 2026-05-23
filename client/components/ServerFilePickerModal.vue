<template>
  <Teleport to="body">
    <div v-if="open" class="picker-backdrop" @click.self="cancel">
      <div class="picker">
        <!-- Top toolbar: navigation + breadcrumb path -->
        <header class="toolbar">
          <button class="nav-btn" :disabled="historyBack.length === 0" @click="goBack" title="Back">←</button>
          <button class="nav-btn" :disabled="!canGoUp" @click="goUp" title="Up one level">↑</button>
          <input
            class="path-input"
            v-model="pathDraft"
            @keydown.enter="navigate(pathDraft)"
            placeholder="Type a path and press Enter"
          />
          <button class="nav-btn" @click="navigate('')" title="Computer / drives">🖥</button>
        </header>

        <!-- Breadcrumb -->
        <div v-if="!isRoot && currentPath" class="breadcrumb">
          <span v-for="(seg, idx) in breadcrumbs" :key="idx">
            <button class="crumb" @click="navigate(seg.path)">{{ seg.name }}</button>
            <span v-if="idx < breadcrumbs.length - 1" class="crumb-sep">›</span>
          </span>
        </div>

        <!-- Main listing -->
        <div class="listing" :class="{ loading }">
          <div v-if="error" class="status error">{{ error }}</div>
          <div v-else-if="loading" class="status">Loading…</div>
          <ul v-else class="entries">
            <li v-for="entry in sortedEntries"
                :key="entry.full_path"
                class="entry"
                :class="[entry.kind, { selected: selected === entry.full_path }]"
                @click="onEntryClick(entry)"
                @dblclick="onEntryActivate(entry)">
              <span class="icon">{{ iconFor(entry) }}</span>
              <span class="name">{{ entry.name }}</span>
              <span v-if="entry.kind === 'file' && entry.size != null" class="size">
                {{ formatBytes(entry.size) }}
              </span>
            </li>
            <li v-if="sortedEntries.length === 0" class="empty">
              <em>{{ filterLabel }} — no matching items in this folder.</em>
            </li>
          </ul>
        </div>

        <!-- Bottom bar: selection + filter + action buttons -->
        <footer class="footer">
          <div class="filename-row">
            <label>File:&nbsp;</label>
            <input class="filename" v-model="filenameDraft" placeholder="(select a file above)" />
          </div>
          <div class="filter-row">
            <select v-model="filter" @change="reload" class="filter">
              <option v-if="filterOptions.includes('audio')" value="audio">Audio files</option>
              <option v-for="opt in filterOptions.filter(o => o !== 'audio' && o !== 'all')"
                      :key="opt" :value="opt">{{ filterDisplay(opt) }}</option>
              <option v-if="filterOptions.includes('all')" value="all">All files</option>
            </select>
            <span class="spacer"></span>
            <button class="btn" @click="cancel">Cancel</button>
            <button class="btn primary" :disabled="!canConfirm" @click="confirm">
              {{ mode === 'directory' ? 'Select folder' : 'Open' }}
            </button>
          </div>
        </footer>
      </div>
    </div>
  </Teleport>
</template>

<!--
  ServerFilePickerModal.vue
  -----------------------------------------------------------------------
  Native-feeling file picker driven entirely by the server's /api/fs/list
  endpoint. Lets the user browse the server's full filesystem (drives,
  network shares, anywhere), so a client running in remote mode can open
  projects/media stored on the server.

  Props:
    open       : boolean   — modal visibility (v-model:open style)
    mode       : 'file' | 'directory'
                            — pick a file (with extension filter) or a folder
    filter     : string    — initial filter token, e.g. 'audio', 'all',
                              '.liveplay,.lpa'
    filterOptions: string[]— filters offered in the dropdown
    startPath  : string    — initial directory (empty = computer root)
    title      : string    — header text shown if you want one

  Emits:
    pick(fullPath: string) — user confirmed a selection
    close                  — user cancelled
-->
<script setup lang="ts">
import { computed, ref, watch } from 'vue';
import { useLiveplayServer } from '~/composables/useLiveplayServer';
import type { ServerFsEntry, ServerFsListing } from '~/types/server';

const props = withDefaults(defineProps<{
  open:           boolean;
  mode?:          'file' | 'directory';
  filter?:        string;
  filterOptions?: string[];
  startPath?:     string;
}>(), {
  mode:          'file',
  filter:        'audio',
  filterOptions: () => ['audio', 'all'],
  startPath:     '',
});

const emit = defineEmits<{
  (e: 'pick', fullPath: string): void;
  (e: 'close'): void;
}>();

const server = useLiveplayServer();

// ---------------------------------------------------------------------------
// Local state — current listing, selection, breadcrumbs, history
// ---------------------------------------------------------------------------
const listing       = ref<ServerFsListing | null>(null);
const loading       = ref(false);
const error         = ref<string | null>(null);
const selected      = ref<string>('');
const filenameDraft = ref<string>('');
const pathDraft     = ref<string>('');
const filter        = ref<string>(props.filter);

// Simple back-history (no forward stack; native dialogs work fine without).
const historyBack = ref<string[]>([]);
let suppressHistory = false;

const currentPath = computed(() => listing.value?.path ?? '');
const isRoot      = computed(() => !!listing.value?.is_root);
const canGoUp     = computed(() =>
  !!listing.value && (!!listing.value.parent || !listing.value.is_root));

const breadcrumbs = computed(() => {
  const path = currentPath.value;
  if (!path) return [];
  // Split on / OR \ and rebuild absolute segments
  const isWin = /^[A-Za-z]:[\\/]/.test(path);
  const parts = path.split(/[\\/]+/).filter(Boolean);
  const segs: { name: string; path: string }[] = [];
  if (isWin) {
    const drive = parts[0]; // e.g. "F:"
    segs.push({ name: drive, path: drive + '\\' });
    let cur = drive + '\\';
    for (let i = 1; i < parts.length; ++i) {
      cur = cur + parts[i] + '\\';
      segs.push({ name: parts[i], path: cur });
    }
  } else {
    let cur = '/';
    segs.push({ name: '/', path: cur });
    for (const p of parts) {
      cur = cur + p + '/';
      segs.push({ name: p, path: cur });
    }
  }
  return segs;
});

const sortedEntries = computed(() => {
  const list = listing.value?.entries ?? [];
  // Drives first, then directories, then files. Alpha within each group.
  const rank: Record<string, number> = { drive: 0, dir: 1, file: 2 };
  return [...list].sort((a, b) => {
    const r = (rank[a.kind] ?? 9) - (rank[b.kind] ?? 9);
    return r !== 0 ? r : a.name.localeCompare(b.name, undefined, { sensitivity: 'base' });
  });
});

const canConfirm = computed(() => {
  if (props.mode === 'directory') {
    // In directory mode the user confirms the current folder, or an
    // explicitly-selected one. Disallow at the computer root.
    return !isRoot.value && (!!selected.value || !!currentPath.value);
  }
  return !!selected.value && fileFor(selected.value)?.kind === 'file';
});

const filterLabel = computed(() => filterDisplay(filter.value));

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
function iconFor(entry: ServerFsEntry): string {
  if (entry.kind === 'drive') return '💽';
  if (entry.kind === 'dir')   return '📁';
  return '📄';
}

function formatBytes(n: number): string {
  if (n < 1024) return `${n} B`;
  const units = ['KB', 'MB', 'GB', 'TB'];
  let v = n / 1024;
  let i = 0;
  while (v >= 1024 && i < units.length - 1) { v /= 1024; i++; }
  return `${v.toFixed(1)} ${units[i]}`;
}

function filterDisplay(f: string): string {
  if (f === 'all')   return 'All files';
  if (f === 'audio') return 'Audio files';
  return f;
}

function fileFor(fullPath: string): ServerFsEntry | undefined {
  return listing.value?.entries.find(e => e.full_path === fullPath);
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------
async function navigate(path: string) {
  loading.value = true;
  error.value   = null;
  try {
    const prev = listing.value?.path ?? '';
    if (!suppressHistory && prev !== path) {
      historyBack.value.push(prev);
    }
    suppressHistory = false;
    listing.value   = await server.listServerPath(path, filter.value);
    pathDraft.value = listing.value.path;
    selected.value  = '';
    filenameDraft.value = '';
  } catch (e: any) {
    error.value = String(e.message || e);
  } finally {
    loading.value = false;
  }
}

function reload() { navigate(currentPath.value); }

function goUp() {
  if (!listing.value) return;
  if (listing.value.parent) navigate(listing.value.parent);
  else if (!listing.value.is_root) navigate('');   // back to drive root
}

function goBack() {
  const prev = historyBack.value.pop();
  if (prev !== undefined) {
    suppressHistory = true;
    navigate(prev);
  }
}

function onEntryClick(entry: ServerFsEntry) {
  selected.value = entry.full_path;
  if (entry.kind === 'file') filenameDraft.value = entry.name;
  else filenameDraft.value = '';
}

function onEntryActivate(entry: ServerFsEntry) {
  if (entry.kind === 'dir' || entry.kind === 'drive') {
    navigate(entry.full_path);
  } else if (entry.kind === 'file') {
    selected.value = entry.full_path;
    confirm();
  }
}

function confirm() {
  if (!canConfirm.value) return;
  if (props.mode === 'directory') {
    // Prefer an explicit folder selection; fall back to the current folder.
    const chosen = (selected.value && fileFor(selected.value)?.kind === 'dir')
      ? selected.value
      : currentPath.value;
    emit('pick', chosen);
  } else {
    emit('pick', selected.value);
  }
}

function cancel() { emit('close'); }

// ---------------------------------------------------------------------------
// Open / close lifecycle
// ---------------------------------------------------------------------------
watch(() => props.open, (o) => {
  if (o) {
    filter.value      = props.filter;
    historyBack.value = [];
    navigate(props.startPath);
  }
});
</script>

<style lang="scss" scoped>
.picker-backdrop {
  position: fixed; inset: 0; z-index: 9100;
  background: rgba(0,0,0,0.6);
  display: flex; align-items: center; justify-content: center;
}
.picker {
  width: min(820px, 95vw);
  height: min(640px, 90vh);
  background: #1a1a1a;
  border: 1px solid #2a2a2a;
  border-radius: 8px;
  display: flex; flex-direction: column;
  color: #ddd;
  font-size: 13px;
  overflow: hidden;
}
.toolbar {
  display: flex; gap: 6px; padding: 10px;
  border-bottom: 1px solid #2a2a2a;
  .nav-btn {
    background: #2a2a2a; border: 1px solid #3a3a3a;
    border-radius: 4px; padding: 4px 10px;
    color: #ddd; cursor: pointer;
    &:hover:not(:disabled) { background: #353535; }
    &:disabled { opacity: 0.4; cursor: not-allowed; }
  }
  .path-input {
    flex: 1;
    background: #1d1d1d; border: 1px solid #333;
    border-radius: 4px; padding: 4px 10px;
    color: #eee; font-family: monospace; font-size: 12px;
  }
}
.breadcrumb {
  padding: 6px 12px; font-size: 11px; color: #aaa;
  border-bottom: 1px solid #222; background: #161616;
  .crumb {
    background: transparent; border: none; color: #9ec5ff; cursor: pointer;
    padding: 2px 4px; font-size: 11px;
    &:hover { text-decoration: underline; }
  }
  .crumb-sep { color: #555; padding: 0 2px; }
}
.listing {
  flex: 1; min-height: 0; overflow: auto;
  background: #161616;
  &.loading { opacity: 0.6; }
}
.entries {
  list-style: none; margin: 0; padding: 0;
}
.entry {
  display: grid; grid-template-columns: 28px 1fr auto;
  gap: 8px; align-items: center; padding: 6px 14px;
  cursor: pointer; border-bottom: 1px solid #1d1d1d;
  &:hover { background: #1f1f1f; }
  &.selected { background: #2a3a55; color: #fff; }
  .icon { text-align: center; }
  .name { color: #eee; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .size { color: #888; font-family: monospace; font-size: 11px; }
  &.dir   .name { color: #ffd58a; }
  &.drive .name { color: #b4dcff; font-weight: 600; }
}
.empty {
  padding: 18px; text-align: center; color: #777; font-style: italic;
}
.status {
  padding: 18px; text-align: center;
  &.error { color: #ff8080; }
}
.footer {
  border-top: 1px solid #2a2a2a;
  padding: 10px 12px;
  display: flex; flex-direction: column; gap: 8px;
  background: #1a1a1a;
}
.filename-row {
  display: flex; align-items: center; gap: 6px;
  .filename {
    flex: 1;
    background: #1d1d1d; border: 1px solid #333;
    border-radius: 4px; padding: 4px 10px;
    color: #eee;
  }
}
.filter-row {
  display: flex; align-items: center; gap: 8px;
  .filter {
    background: #1d1d1d; border: 1px solid #333; color: #eee;
    padding: 4px 8px; border-radius: 4px;
  }
  .spacer { flex: 1; }
  .btn {
    background: #2a2a2a; border: 1px solid #3a3a3a;
    border-radius: 4px; padding: 6px 16px; color: #ddd; cursor: pointer;
    &:hover:not(:disabled) { background: #353535; }
    &:disabled { opacity: 0.5; cursor: not-allowed; }
    &.primary { background: #2a5e9a; border-color: #2a5e9a; }
  }
}
</style>

<template>
  <Teleport to="body">
    <div v-if="open" class="modal-backdrop" @click.self="close">
      <div class="modal">
        <header>
          <h2>{{ t('importAudio.title') }}</h2>
          <button class="x" @click="close">✕</button>
        </header>

        <!-- Source mode toggle. Only rendered when the server is on a
             different machine; otherwise "upload" makes no sense and we
             skip the tabs entirely. -->
        <div v-if="!server.isLocalServer.value" class="tabs">
          <button class="tab" :class="{ active: tab === 'server' }" @click="tab = 'server'">
            {{ t('importAudio.tabServer') }}
          </button>
          <button class="tab" :class="{ active: tab === 'upload' }" @click="tab = 'upload'">
            {{ t('importAudio.tabUpload') }}
          </button>
        </div>

        <!-- "On server" tab — browses the server's filesystem via /api/fs/list -->
        <section v-if="tab === 'server'" class="pane">
          <ServerFileBrowser :start-path="server.serverUrl ? '' : ''" @select="onServerPick" />
          <p class="hint">{{ t('importAudio.serverHint') }}</p>

          <!-- Local file picker: only shown on local server (Electron only) -->
          <template v-if="server.isLocalServer.value && hasElectron">
            <div class="divider">{{ t('importAudio.orFromComputer') }}</div>
            <div class="row">
              <button class="btn primary" :disabled="pickingLocal" @click="pickLocal">
                <span class="material-symbols-rounded" style="font-size:16px;vertical-align:middle;">folder_open</span>
                {{ pickingLocal ? t('importAudio.uploading') : t('importAudio.chooseFiles') }}
              </button>
            </div>
            <ul v-if="localPicked.length" class="uploaded">
              <li v-for="(p, i) in localPicked"
                  :key="p"
                  :class="{ selected: selectedLocal.includes(p) }"
                  @click="toggleLocal(p, i, $event)">
                <span class="icon material-symbols-rounded">audio_file</span>
                <span class="name">{{ basename(p) }}</span>
              </li>
            </ul>
            <div v-if="localPicked.length" class="list-footer">
              <button class="btn primary" :disabled="!selectedLocal.length" @click="importLocalSelected">
                {{ t('importAudio.importSelected') }}<span v-if="selectedLocal.length"> ({{ selectedLocal.length }})</span>
              </button>
            </div>
          </template>
        </section>

        <!-- "Upload" tab — native dialog, then multipart POST to /api/upload -->
        <section v-else class="pane">
          <p>{{ t('importAudio.uploadIntro') }}</p>

          <div class="row">
            <button class="btn primary" :disabled="uploading" @click="pickAndUpload">
              <span class="material-symbols-rounded" style="font-size:16px;vertical-align:middle;">folder_open</span>
              {{ uploading ? t('importAudio.uploading') : t('importAudio.chooseFiles') }}
            </button>
            <span v-if="uploadStatus" class="status">{{ uploadStatus }}</span>
          </div>

          <ul v-if="uploadedThisSession.length" class="uploaded">
            <li v-for="(p, i) in uploadedThisSession"
                :key="p"
                :class="{ selected: selectedUploaded.includes(p) }"
                @click="toggleUploaded(p, i, $event)">
              <span class="icon material-symbols-rounded">audio_file</span>
              <span class="name">{{ basename(p) }}</span>
            </li>
          </ul>
          <div v-if="uploadedThisSession.length" class="list-footer">
            <button class="btn primary" :disabled="!selectedUploaded.length" @click="importUploadedSelected">
              {{ t('importAudio.importSelected') }}<span v-if="selectedUploaded.length"> ({{ selectedUploaded.length }})</span>
            </button>
          </div>
        </section>
      </div>
    </div>
  </Teleport>
</template>

<!--
  AudioImportModal.vue
  -----------------------------------------------------------------------
  Replaces the native OS file dialog for audio import. Two tabs:
   * "On server"  — browse the server's filesystem with ServerFileBrowser.
   * "Upload"     — use Electron's native dialog to pick local files, then
                    stream them to the server via /api/upload (multipart).

  Emits:
    pick(serverPaths: string[]) — caller proceeds to create AudioItems for
                                  each selected server-side path (batched).
    close                       — user dismissed the modal.

  Notes:
    The user explicitly chose 'Upload to server's media_root' so that
    both local and remote-server modes behave identically downstream.
-->
<script setup lang="ts">
import { ref } from 'vue';
import { useLiveplayServer } from '~/composables/useLiveplayServer';
import ServerFileBrowser from '~/components/ServerFileBrowser.vue';

const props = defineProps<{ open: boolean }>();
const emit  = defineEmits<{
  (e: 'pick', serverPaths: string[]): void;
  (e: 'close'): void;
}>();

const server = useLiveplayServer();
const { t }  = useLocalization();
// When the server is on this same machine, "Upload from this computer" is
// meaningless — we just want the server file browser.
const tab    = ref<'server' | 'upload'>('server');

const uploading           = ref(false);
const uploadStatus        = ref<string>('');
const uploadedThisSession = ref<string[]>([]);
const selectedUploaded    = ref<string[]>([]);
const uploadedAnchor      = { i: -1 };

// Local file picker (used when server is local — same machine, so local paths = server paths)
const hasElectron = !!(globalThis as any).electronAPI?.selectAudioFiles;
const localPicked  = ref<string[]>([]);
const selectedLocal = ref<string[]>([]);
const localAnchor   = { i: -1 };
const pickingLocal = ref(false);

function close()       { emit('close'); }
function basename(p: string): string { return p.split(/[\\/]/).pop() || p; }

// Server file browser emits a batch of already-on-server paths.
function onServerPick(serverPaths: string[]) {
  if (serverPaths.length) emit('pick', serverPaths);
}

function importLocalSelected() {
  if (selectedLocal.value.length) emit('pick', [...selectedLocal.value]);
}

function importUploadedSelected() {
  if (selectedUploaded.value.length) emit('pick', [...selectedUploaded.value]);
}

// Shared click-selection for the staging lists (local picks / uploaded files).
// Plain click toggles membership so a batch is easy to build without modifiers;
// Ctrl/Cmd also toggles, Shift extends a range from the last anchor. Returns
// the new selection array.
function clickSelect(
  items: string[],
  current: string[],
  anchor: { i: number },
  item: string,
  index: number,
  e: MouseEvent,
): string[] {
  if (e.shiftKey && anchor.i >= 0) {
    const [lo, hi] = anchor.i < index ? [anchor.i, index] : [index, anchor.i];
    const slice = items.slice(lo, hi + 1);
    return (e.ctrlKey || e.metaKey)
      ? Array.from(new Set([...current, ...slice]))
      : slice;
  }
  anchor.i = index;
  return current.includes(item)
    ? current.filter(p => p !== item)
    : [...current, item];
}

function toggleLocal(item: string, index: number, e: MouseEvent) {
  selectedLocal.value = clickSelect(localPicked.value, selectedLocal.value, localAnchor, item, index, e);
}

function toggleUploaded(item: string, index: number, e: MouseEvent) {
  selectedUploaded.value = clickSelect(uploadedThisSession.value, selectedUploaded.value, uploadedAnchor, item, index, e);
}

async function pickLocal() {
  const api: any = (globalThis as any).electronAPI;
  if (!api?.selectAudioFiles) return;
  pickingLocal.value = true;
  try {
    const paths: string[] | null = await api.selectAudioFiles();
    if (paths?.length) {
      for (const p of paths) {
        if (!localPicked.value.includes(p)) {
          localPicked.value.push(p);
          selectedLocal.value.push(p);   // pre-select newly picked files
        }
      }
    }
  } finally {
    pickingLocal.value = false;
  }
}

// Native dialog → multipart upload → register paths under the server's media_root.
// Falls back gracefully if not running inside Electron.
async function pickAndUpload() {
  const api: any = (globalThis as any).electronAPI;
  if (!api?.selectAudioFiles) {
    uploadStatus.value = t('importAudio.desktopOnly');
    return;
  }

  const localPaths: string[] | null = await api.selectAudioFiles();
  if (!localPaths || localPaths.length === 0) return;

  uploading.value = true;
  try {
    for (let i = 0; i < localPaths.length; ++i) {
      const lp = localPaths[i];
      uploadStatus.value = t('importAudio.uploadingProgress',
        { i: i + 1, total: localPaths.length, name: basename(lp) });

      // Read raw bytes through Electron, then wrap as a File for fetch().
      // (This avoids needing the renderer to have direct disk access.)
      // readAudioFile returns the binary as a number[]; readFile would
      // try to decode utf8 and corrupt audio payloads.
      const result = await api.readAudioFile(lp);
      if (!result?.success || !result.data) {
        console.warn('[import] readAudioFile failed for', lp, result?.error);
        continue;
      }
      const bytes = new Uint8Array(result.data);
      const file = new File([bytes], basename(lp));

      const out = await server.uploadFile(file, basename(lp));
      if (out?.saved?.length) {
        for (const savedPath of out.saved) {
          uploadedThisSession.value.push(savedPath);
          selectedUploaded.value.push(savedPath);   // pre-select for one-click import
        }
      }
    }
    uploadStatus.value = t('importAudio.uploadedCount', { count: localPaths.length });
  } catch (e: any) {
    uploadStatus.value = t('importAudio.uploadFailed', { error: e?.message ?? e });
  } finally {
    uploading.value = false;
  }
}
</script>

<style lang="scss" scoped>
.modal-backdrop {
  position: fixed; inset: 0;
  background: rgba(0,0,0,0.6);
  display: flex; align-items: center; justify-content: center;
  z-index: 9000;
}
.modal {
  width: min(720px, 92vw);
  background: #1a1a1a;
  border: 1px solid #2a2a2a;
  border-radius: 8px;
  padding: 18px;
  color: #ddd;
  display: flex; flex-direction: column; gap: 12px;

  header { display: flex; justify-content: space-between; align-items: center; }
  h2 { margin: 0; font-size: 16px; }
  .x { background: transparent; border: none; color: #aaa; cursor: pointer; font-size: 16px; }

  .tabs {
    display: flex; gap: 4px;
    border-bottom: 1px solid #2a2a2a;
  }
  .tab {
    flex: 1; background: transparent; border: none; cursor: pointer;
    padding: 8px 12px; color: #888; font-size: 13px;
    border-bottom: 2px solid transparent;
    &:hover  { color: #ccc; }
    &.active { color: #fff; border-bottom-color: var(--color-accent); }
  }

  .pane { display: flex; flex-direction: column; gap: 10px; }
  .hint { font-size: 11px; color: #888; margin: 0; }
  .divider {
    display: flex; align-items: center; gap: 8px;
    font-size: 11px; color: #666; margin: 4px 0;
    &::before, &::after { content: ''; flex: 1; border-top: 1px solid #2a2a2a; }
  }

  .btn {
    background: #2a2a2a; border: 1px solid #3a3a3a; border-radius: 4px;
    padding: 6px 12px; color: #ddd; cursor: pointer;
    display: inline-flex; align-items: center; gap: 4px;
    &:hover:not(:disabled) { background: #353535; }
    &:disabled { opacity: 0.5; cursor: not-allowed; }
    &.primary { background: var(--color-accent); border-color: var(--color-accent); color: #fff; }
    &.small   { padding: 2px 8px; font-size: 12px; }
  }
  .row { display: flex; gap: 10px; align-items: center; }
  .status { font-size: 12px; color: #aaa; }

  .uploaded {
    list-style: none; margin: 0; padding: 0;
    border: 1px solid #2a2a2a; border-radius: 4px; background: #161616;
    max-height: 200px; overflow: auto;
    li {
      display: grid; grid-template-columns: 28px 1fr; gap: 8px;
      align-items: center; padding: 6px 10px;
      border-bottom: 1px solid #222;
      cursor: pointer; user-select: none;
      &:last-child { border-bottom: none; }
      &:hover { background: #1f1f1f; }
      // Files are selectable here, so their icon takes the accent colour.
      .icon { font-size: 18px; color: var(--color-accent); text-align: center; }
      .name { color: #fff; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
      &.selected {
        background: var(--color-accent);
        .icon, .name { color: #fff; }
      }
    }
  }
  .list-footer {
    display: flex; justify-content: flex-end;
  }
}
</style>

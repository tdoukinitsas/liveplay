// =====================================================================
// useMissingMedia.ts
// ---------------------------------------------------------------------
// Which audio items' media files the SERVER can't find (issue #80), so the
// playlist and cart can badge them and the operator can relink them before
// the show rather than discovering a silent cue during it.
//
// The server is the only one who can answer (it resolves media against its
// own filesystem), so this mirrors GET /api/media/missing. It refreshes:
//   * when the connection or the open project changes,
//   * shortly after any doc_patch that can add, move or relink media,
//   * every 15 s, because files can vanish (or come back) behind our back.
//
// Relinking:
//   * locate(folder)  — server scans a folder for same-named files and relinks
//                       every match (copying outside files into media/).
//   * the Properties panel's Replace Media covers one item at a time.
// =====================================================================
import type { MissingMediaEntry } from '~/composables/useLiveplayServer';

const POLL_MS = 15_000;
const DEBOUNCE_MS = 400;
const MEDIA_OPS = new Set([
  'item_added', 'item_updated', 'item_removed', 'project_changed', 'items_reordered',
]);

let _wired = false;

export const useMissingMedia = () => {
  const server = useLiveplayServer();
  const { currentProject } = useProject();

  const missing = useState<Record<string, MissingMediaEntry>>('missingMedia', () => ({}));
  const missingList = computed(() => Object.values(missing.value));
  const missingCount = computed(() => missingList.value.length);
  const isMissing = (uuid: string | undefined | null) => !!uuid && !!missing.value[uuid];

  async function refresh() {
    if (!server.connected.value || !currentProject.value) {
      missing.value = {};
      return;
    }
    try {
      const list = await server.fetchMissingMedia();
      const next: Record<string, MissingMediaEntry> = {};
      for (const e of list) if (e.uuid) next[e.uuid] = e;
      missing.value = next;
    } catch (e) {
      // An older server has no such route; keep the last answer.
      console.warn('[missing-media] refresh failed:', e);
    }
  }

  let debounce: ReturnType<typeof setTimeout> | null = null;
  function scheduleRefresh() {
    if (debounce) clearTimeout(debounce);
    debounce = setTimeout(() => { debounce = null; void refresh(); }, DEBOUNCE_MS);
  }

  /** Relink every missing item to a same-named file under a SERVER folder. */
  async function locate(folder: string) {
    const result = await server.locateMissingMedia(folder, true);
    // A relinked item's old waveform may be for a file that no longer exists;
    // the non-forced request reuses the cache when it's still valid.
    for (const r of result.relinked) {
      const path = r.patch?.mediaServerPath;
      if (typeof path === 'string' && path) {
        server.requestWaveformGeneration(path, r.uuid).catch((e: Error) =>
          console.warn('[missing-media] waveform request failed:', e));
      }
    }
    await refresh();
    return result;
  }

  if (!_wired && import.meta.client) {
    _wired = true;
    server.onDocPatch((p: any) => {
      if (p && MEDIA_OPS.has(p.op)) scheduleRefresh();
    });
    watch(() => server.connected.value, scheduleRefresh);
    watch(() => currentProject.value?.folderPath, scheduleRefresh);
    setInterval(() => { if (server.connected.value && currentProject.value) void refresh(); }, POLL_MS);
    scheduleRefresh();
  }

  return { missing, missingList, missingCount, isMissing, refresh, scheduleRefresh, locate };
};

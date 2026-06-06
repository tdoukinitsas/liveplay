// =====================================================================
// useLiveplayServer.ts
// ---------------------------------------------------------------------
// Central client for the LivePlay C++ server (post-Milestone 3).
//
// One instance per app. Owns:
//   * the WebSocket connection at /ws (with auto-reconnect)
//   * a thin typed REST wrapper for /api/*
//   * reactive state: connection status, cue list, mixer channels,
//     device list — kept in sync with the server.
//
// All audio playback in the refactored client flows through here. Howler
// and WaveSurfer have been removed from the dependency tree; transport
// commands are JSON WebSocket messages, waveforms are rendered from the
// server's downsampled buckets, meters come over the WS broadcast.
//
// Usage (anywhere in a component):
//   const server = useLiveplayServer();
//   server.connect();                    // safe to call repeatedly
//   server.play(cueId);                  // fire WS command
//   await server.fetchCues();            // populate state.cues
//   watch(server.connected, ...);        // react to connection state
// =====================================================================
import { reactive, ref, shallowRef, computed } from 'vue';
import type {
  CueId,
  DeviceId,
  MasterChannelIndex,
  MetersBroadcast,
  MixerChannelId,
  ServerCue,
  ServerDeviceInfo,
  ServerFsListing,
  ServerMixerChannel,
  ServerWaveform,
} from '~/types/server';

// ---------------------------------------------------------------------
// Singleton — created lazily on first useLiveplayServer() call.
// ---------------------------------------------------------------------
let _instance: ReturnType<typeof createClient> | null = null;

export function useLiveplayServer() {
  if (!_instance) _instance = createClient();
  return _instance;
}

function createClient() {
  // ---- Server URL config (persisted via localStorage) ---------------
  const defaultUrl = (typeof window !== 'undefined' &&
                      window.localStorage?.getItem('liveplay.serverUrl')) ||
                     'http://127.0.0.1:4480';
  const serverUrl = ref<string>(defaultUrl);

  const httpBase = computed(() => serverUrl.value.replace(/\/+$/, ''));
  const wsUrl    = computed(() =>
    httpBase.value.replace(/^http/i, 'ws') + '/ws');

  function setServerUrl(url: string) {
    serverUrl.value = url;
    if (typeof window !== 'undefined') {
      window.localStorage?.setItem('liveplay.serverUrl', url);
    }
    // URL change → treat as a brand-new session. Force re-fetch on next
    // onopen by clearing the first-connect guard.
    hasEverConnected = false;
    disconnect();
    connect();
  }

  // ---- Reactive state -----------------------------------------------
  const connected     = ref(false);
  const reconnecting  = ref(false);
  const lastError     = ref<string | null>(null);
  const cues          = ref<ServerCue[]>([]);
  const mixerChannels = ref<ServerMixerChannel[]>([]);
  const devices       = ref<ServerDeviceInfo[]>([]);

  // Live meter snapshot, replaced wholesale on each WS frame. shallowRef
  // because we never mutate the object — only swap the whole reference.
  const meters = shallowRef<MetersBroadcast | null>(null);

  // Optional subscribers (e.g. useLiveMeters composable) can register a
  // callback to be invoked synchronously on every WS frame for sub-frame
  // smoothing if they need it.
  type MetersSubscriber = (m: MetersBroadcast) => void;
  const metersSubscribers = new Set<MetersSubscriber>();
  function onMeters(cb: MetersSubscriber): () => void {
    metersSubscribers.add(cb);
    return () => metersSubscribers.delete(cb);
  }

  // Subscribers for cue transport-state transitions emitted by the server.
  // Payload: { cue_id, transport (0=Stopped,1=Playing,2=FadingIn,3=FadingOut), playhead_seconds }
  type CueStatePayload = { cue_id: string; transport: number; playhead_seconds: number };
  type CueStateSubscriber = (s: CueStatePayload) => void;
  const cueStateSubscribers = new Set<CueStateSubscriber>();
  function onCueState(cb: CueStateSubscriber): () => void {
    cueStateSubscribers.add(cb);
    return () => cueStateSubscribers.delete(cb);
  }

  // Subscribers for the server's playback_snapshot message — sent once on
  // every (re)connect so the client can rebuild its idea of what's playing,
  // what's "Up Next", and any active preview without waiting for the next
  // transport edge to fire.
  type PlaybackSnapshot = {
    cues: Array<{ cue_id: string; transport: number; playhead_seconds: number }>;
    next_item_uuid: string;
    preview: { item_uuid: string; cue_id: string };
  };
  type PlaybackSnapshotSubscriber = (s: PlaybackSnapshot) => void;
  const playbackSnapshotSubscribers = new Set<PlaybackSnapshotSubscriber>();
  function onPlaybackSnapshot(cb: PlaybackSnapshotSubscriber): () => void {
    playbackSnapshotSubscribers.add(cb);
    return () => playbackSnapshotSubscribers.delete(cb);
  }

  // Subscribers for multi-client doc_patch fan-out events.
  // Payload: { type: 'doc_patch', op: 'item_added'|'item_updated'|... , ... }
  type DocPatchSubscriber = (p: any) => void;
  const docPatchSubscribers = new Set<DocPatchSubscriber>();
  function onDocPatch(cb: DocPatchSubscriber): () => void {
    docPatchSubscribers.add(cb);
    return () => docPatchSubscribers.delete(cb);
  }

  // ---- WebSocket ----------------------------------------------------
  let ws: WebSocket | null = null;
  let reconnectTimer: ReturnType<typeof setTimeout> | null = null;
  let reconnectDelay = 1500;         // start higher; backs off to 10 s
  // True after the *very first* successful onopen for this session. Used to
  // skip the expensive triple-fetch (cues, mixers, devices) on every reconnect
  // — those don't change just because the WS bounced.
  let hasEverConnected = false;
  // Count of consecutive failed reconnect attempts. Resets on every onopen.
  // Used to gate the "we're really down" UI signal: a single bounce shouldn't
  // pop a modal, but four failed retries with growing backoff almost certainly
  // means the server is gone.
  const failedReconnectAttempts = ref(0);
  // True once we've decided the server is gone. Cleared on a successful
  // reconnect. UI binds to this to show the connection-lost modal.
  const connectionLost = ref(false);
  const CONNECTION_LOST_THRESHOLD = 4;

  function scheduleReconnect() {
    if (reconnectTimer) return;
    reconnecting.value = true;
    reconnectTimer = setTimeout(() => {
      reconnectTimer = null;
      reconnectDelay = Math.min(reconnectDelay * 2, 10000);
      connect();
    }, reconnectDelay);
  }

  // Force an immediate reconnect attempt. Resets backoff and the
  // "connection lost" flag so the UI dismisses the disconnect modal.
  // Used by the Reconnect button.
  function forceReconnect() {
    if (reconnectTimer) { clearTimeout(reconnectTimer); reconnectTimer = null; }
    reconnectDelay = 1500;
    failedReconnectAttempts.value = 0;
    connectionLost.value = false;
    disconnect();
    connect();
  }

  function connect() {
    if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) {
      return;
    }
    try {
      // eslint-disable-next-line no-console
      console.log('[liveplay] connecting to', wsUrl.value);
      ws = new WebSocket(wsUrl.value);
    } catch (e) {
      lastError.value = String(e);
      // eslint-disable-next-line no-console
      console.error('[liveplay] WebSocket constructor threw:', e);
      scheduleReconnect();
      return;
    }

    ws.onopen = () => {
      connected.value = true;
      reconnecting.value = false;
      reconnectDelay = 1500;
      lastError.value = null;
      failedReconnectAttempts.value = 0;
      connectionLost.value = false;
      // Only refresh REST-backed catalogues on the very first connection.
      // Reconnects (e.g. transient Crow WS close-frame issues) don't change
      // those tables, so re-fetching every time produced a request storm
      // that masked any actual UI work.
      // Always re-check whether we're talking to a server on this same
      // machine — the URL might look remote (LAN IP) but route to loopback,
      // and /api/whoami is the only authoritative answer.
      void refreshIsLocalServer();
      if (!hasEverConnected) {
        hasEverConnected = true;
        void Promise.allSettled([fetchCues(), fetchMixerChannels(), fetchDevices()]);
      } else {
        // On reconnect, the server's playback_snapshot (sent immediately
        // after the WS open) covers transport/up-next/preview state, but
        // the project document itself may have been mutated by another
        // client while we were offline. Refetch the cue catalogue so any
        // cues added in the meantime show up; subscribers (useProject)
        // can listen on onPlaybackSnapshot to do a header re-sync as well.
        void fetchCues().catch(() => {});
      }
    };

    ws.onclose = () => {
      const wasConnected = connected.value;
      connected.value = false;
      // Only count this as a "failed reconnect" if we never successfully
      // connected before *this* attempt — i.e., the WebSocket just bounced
      // straight to close without an onopen in between. Pre-handshake
      // failures are the strong "server is gone" signal we want to catch.
      if (!wasConnected && hasEverConnected) {
        failedReconnectAttempts.value++;
        if (failedReconnectAttempts.value >= CONNECTION_LOST_THRESHOLD) {
          connectionLost.value = true;
        }
      }
      scheduleReconnect();
    };

    ws.onerror = (ev) => {
      lastError.value = 'WebSocket error';
      // onerror is followed by onclose; reconnection happens there.
      void ev;
    };

    ws.onmessage = (ev) => {
      let payload: any;
      try { payload = JSON.parse(ev.data); } catch { return; }
      if (!payload || typeof payload !== 'object' || !payload.type) return;

      switch (payload.type) {
        case 'meters': {
          meters.value = payload as MetersBroadcast;
          for (const cb of metersSubscribers) cb(payload as MetersBroadcast);
          break;
        }
        case 'playback_snapshot': {
          // Update each known cue's transport in place so any UI bound to
          // `cues[i].transport` repaints. The cue may not be in the local
          // list yet on a cold reconnect — that's handled by the broader
          // refetch below, which re-runs the catalogue fetches and then
          // a second snapshot is applied as cue_state edges resume.
          const snap = payload as PlaybackSnapshot;
          for (const c of snap.cues ?? []) {
            const idx = cues.value.findIndex(x => x.id === c.cue_id);
            if (idx >= 0) {
              cues.value[idx].transport = c.transport as any;
              cues.value[idx].playhead_seconds = c.playhead_seconds;
            }
            // Also fire as a synthetic cue_state event so subscribers
            // (useAudioEngine) see "Playing" for items the snapshot lists.
            for (const cb of cueStateSubscribers) cb(c);
          }
          // Restore per-output-channel gains from snapshot.
          for (const g of (snap as any).output_channel_gains ?? []) {
            outputChannelGains.value[g.channel] = g.db;
          }
          for (const cb of playbackSnapshotSubscribers) cb(snap);
          break;
        }
        case 'cue_state': {
          // Mutate only the changed properties rather than replacing the whole
          // object.  Replacing triggers Vue to invalidate every component that
          // holds a reference to the old cue object; in-place mutation lets Vue
          // track the narrower `transport` / `playhead_seconds` dependencies
          // and avoids a broad re-render cascade across all PlaylistItems.
          const idx = cues.value.findIndex(c => c.id === payload.cue_id);
          if (idx >= 0) {
            cues.value[idx].transport        = payload.transport;
            cues.value[idx].playhead_seconds = payload.playhead_seconds;
          }
          // Notify subscribers (e.g. useAudioEngine cleans up activeCues on stop).
          for (const cb of cueStateSubscribers) cb(payload as CueStatePayload);
          break;
        }
        case 'doc_patch': {
          // Handle output_channel_gain_changed locally before fanning out.
          if (payload.op === 'output_channel_gain_changed' &&
              typeof payload.channel === 'number' &&
              typeof payload.db === 'number') {
            outputChannelGains.value = {
              ...outputChannelGains.value,
              [payload.channel]: payload.db,
            };
          }
          // Multi-client mirror: another client (or the local mutator
          // itself) just changed something. Hand off to subscribers
          // (useProject installs one that applies the patch under
          // isHydrating so the local diff-watcher doesn't echo it back).
          for (const cb of docPatchSubscribers) cb(payload);
          break;
        }
        case 'pong':
          break;
        case 'error':
          lastError.value = String(payload.message || 'server error');
          break;
      }
    };
  }

  function disconnect() {
    if (reconnectTimer) {
      clearTimeout(reconnectTimer);
      reconnectTimer = null;
    }
    if (ws) {
      ws.onopen = ws.onclose = ws.onerror = ws.onmessage = null;
      try { ws.close(); } catch {}
      ws = null;
    }
    connected.value = false;
    reconnecting.value = false;
  }

  function wsSend(payload: object) {
    const body = JSON.stringify(payload);
    if (ws && ws.readyState === WebSocket.OPEN) {
      // eslint-disable-next-line no-console
      console.log('[liveplay] WS send:', body);
      ws.send(body);
    } else {
      // Loudly flag — silent drops are the most painful class of WS bug.
      const state = ws ? ws.readyState : 'no-ws';
      // eslint-disable-next-line no-console
      console.warn('[liveplay] WS send DROPPED (readyState=' + state + '):', body);
    }
  }

  // ---- Transport (WS — low-latency) ---------------------------------
  function play(cue: CueId)             { wsSend({ type: 'play',     cue_id: cue }); }
  function stop(cue: CueId)             { wsSend({ type: 'stop',     cue_id: cue }); }
  function stopAll(fadeMs = 0)          { wsSend({ type: 'stop_all', fade_ms: fadeMs }); }
  function setGainDb(cue: CueId, db: number)
                                        { wsSend({ type: 'gain', cue_id: cue, db }); }
  function setFade(cue: CueId, inMs: number, outMs: number)
                                        { wsSend({ type: 'fade', cue_id: cue,
                                                   in_ms: inMs, out_ms: outMs }); }
  function ping()                       { wsSend({ type: 'ping' }); }

  // ---- REST helpers -------------------------------------------------
  async function rest<T = any>(path: string, init?: RequestInit): Promise<T> {
    const url = httpBase.value + path;
    // eslint-disable-next-line no-console
    console.log('[liveplay] rest start:', init?.method || 'GET', url);
    let res: Response;
    try {
      res = await fetch(url, {
        headers: { 'Content-Type': 'application/json' },
        ...init,
      });
    } catch (e) {
      // eslint-disable-next-line no-console
      console.error('[liveplay] rest fetch threw:', e);
      throw e;
    }
    // eslint-disable-next-line no-console
    console.log('[liveplay] rest headers:', res.status, res.statusText, 'for', url);
    if (!res.ok) {
      const text = await res.text().catch(() => res.statusText);
      throw new Error(`${res.status} ${res.statusText} — ${text}`);
    }
    let parsed: T;
    try {
      parsed = await (res.json() as Promise<T>);
    } catch (e) {
      // eslint-disable-next-line no-console
      console.error('[liveplay] rest json() failed for', url, ':', e);
      throw e;
    }
    // eslint-disable-next-line no-console
    console.log('[liveplay] rest done:', url);
    return parsed;
  }

  async function fetchCues() {
    cues.value = await rest<ServerCue[]>('/api/cues');
  }
  async function fetchMixerChannels() {
    mixerChannels.value = await rest<ServerMixerChannel[]>('/api/mixers');
  }
  async function fetchDevices() {
    devices.value = await rest<ServerDeviceInfo[]>('/api/devices');
  }
  async function fetchProject() {
    return rest<any>('/api/project');
  }
  // Lightweight header — everything except the items tree. Used by the
  // "open project" flow so the workspace shell paints before the items
  // array has even started downloading.
  async function fetchProjectHeader() {
    return rest<{
      name: string;
      version: string;
      folderPath: string;
      createdAt: string;
      lastModified: string;
      theme: any;
      settings: any;
      cartItems: any[];
      cartSlotKeys: any;
      playbackKeys: any;
      cartOnlyItems: any[];
      itemCount: number;
      hasOpenProject: boolean;
      server: { projectFilePath: string; mediaRoot: string;
                audioLoading: boolean; audioLoaded: number; audioTotal: number };
    }>('/api/project/header');
  }
  // Paged items. Caller drives the loop; we keep this stateless so it
  // composes with the open-project streaming logic in useProject.
  async function fetchProjectItemsPage(offset = 0, limit = 100) {
    return rest<{
      offset: number; limit: number; total: number; items: any[];
    }>(`/api/project/items?offset=${offset}&limit=${limit}`);
  }
  async function fetchProjectProgress() {
    return rest<{ loading: boolean; loaded: number; total: number }>(
      '/api/project/progress');
  }
  async function loadProjectFromPath(path: string) {
    return rest<any>('/api/project/load', {
      method: 'POST',
      body: JSON.stringify({ path }),
    }).then(p => { fetchCues(); fetchMixerChannels(); return p; });
  }
  async function loadProjectFromDocument(document: any) {
    return rest<any>('/api/project/load', {
      method: 'POST',
      body: JSON.stringify({ document }),
    }).then(p => { fetchCues(); fetchMixerChannels(); return p; });
  }
  async function saveProjectTo(path?: string, document?: any) {
    // Authoritative-save: when the caller provides the latest document, send
    // it along so the server replaces its in-memory copy (and re-mirrors per-
    // cue properties to the audio engine) before writing to disk. Belt-and-
    // suspenders against the granular item-diff watcher missing an edit and
    // letting the file save with stale fades / volume / behaviour values.
    const body: Record<string, any> = {};
    if (path) body.path = path;
    if (document) body.document = document;
    return rest<any>('/api/project/save', {
      method: 'POST',
      body: JSON.stringify(body),
    });
  }
  async function repairProject(): Promise<{ repaired: boolean; issues: string[] }> {
    return rest<any>('/api/project/repair', { method: 'POST', body: '{}' });
  }
  // Close the project on the server (reset to no-project state). Mirrors the
  // local closeProject() in useProject so the server doesn't keep playing /
  // holding a project we've dismissed.
  async function closeProjectOnServer(): Promise<{ closed: boolean }> {
    return rest<any>('/api/project/close', { method: 'POST', body: '{}' });
  }

  // True when the configured server is running on this same machine. Used
  // by import/export flows to decide whether to show the dual-dialog choice
  // (server vs. this computer) — picking files from "this computer" is
  // meaningless when the server IS this computer.
  //
  // We can't rely on hostname alone: the user may connect to a server on
  // their own machine via its LAN IP (192.168.x.x) instead of localhost.
  // The authoritative answer comes from /api/whoami, which reports back
  // whether the server saw the request arrive on its loopback interface.
  // The reactive `isLocalServer` ref is updated on every reconnect.
  const isLocalServer = ref<boolean>(false);

  // Synchronous loopback-hostname check as a fast-path — used as the
  // initial value before /api/whoami answers, and as the fallback when the
  // server isn't reachable yet.
  function urlLooksLocal(url: string): boolean {
    try {
      const h = new URL(url).hostname.toLowerCase();
      return h === 'localhost' || h === '127.0.0.1' || h === '::1' || h === '[::1]';
    } catch { return false; }
  }
  isLocalServer.value = urlLooksLocal(serverUrl.value);

  async function refreshIsLocalServer(): Promise<void> {
    try {
      const r = await rest<{ clientIp: string; isLocal: boolean }>('/api/whoami');
      isLocalServer.value = !!r.isLocal;
    } catch {
      // Network blip — fall back to URL heuristic so we still have an answer.
      isLocalServer.value = urlLooksLocal(serverUrl.value);
    }
  }

  // Package a project folder server-side into a .lpa archive.
  //  * outputPath set → archive written there on the server; no download token.
  //  * outputPath empty → archive staged in server temp dir; response carries
  //    a one-shot download token the client redeems via downloadArchive().
  async function exportProjectArchive(folderPath: string, projectName?: string,
                                      outputPath?: string) {
    return rest<{
      archivePath: string;
      size: number;
      downloadToken?: string;
      downloadFilename?: string;
    }>('/api/project/export', {
      method: 'POST',
      body: JSON.stringify({
        folderPath,
        projectName: projectName ?? '',
        outputPath:  outputPath  ?? '',
      }),
    });
  }

  // Redeem a one-shot download token and return the .lpa bytes as a Blob.
  // The server deletes the temp file after streaming, so a token is single-use.
  async function downloadArchive(token: string): Promise<Blob> {
    const res = await fetch(httpBase.value + '/api/file/download?token=' +
                            encodeURIComponent(token));
    if (!res.ok) {
      const text = await res.text().catch(() => res.statusText);
      throw new Error(`download failed: ${res.status} ${text}`);
    }
    return await res.blob();
  }

  // Upload a .lpa archive from the client and have the server extract it
  // into `extractPath` (server-side absolute path).
  async function importProjectArchiveUpload(file: File | Blob,
                                            extractPath: string,
                                            filename?: string) {
    const fd = new FormData();
    fd.append('file', file, filename ?? (file as File).name ?? 'import.lpa');
    fd.append('extractPath', extractPath);
    const res = await fetch(httpBase.value + '/api/project/import', {
      method: 'POST',
      body: fd,
    });
    if (!res.ok) {
      const text = await res.text().catch(() => res.statusText);
      throw new Error(`import upload failed: ${res.status} ${text}`);
    }
    return res.json() as Promise<{
      extractPath: string;
      projectFiles: string[];
    }>;
  }

  // Have the server extract a .lpa archive that already exists on its
  // filesystem (chosen via the server file browser).
  async function importProjectArchiveFromServer(archivePath: string,
                                                extractPath: string) {
    return rest<{ extractPath: string; projectFiles: string[] }>(
      '/api/project/import',
      {
        method: 'POST',
        body: JSON.stringify({ archivePath, extractPath }),
      });
  }
  // PUT the full project document. Server replaces in-memory state and
  // re-mirrors audio items into the engine.
  async function replaceProjectDocument(document: any) {
    return rest<any>('/api/project/document', {
      method: 'PUT',
      body: JSON.stringify(document),
    }).then(p => { fetchCues(); fetchMixerChannels(); return p; });
  }

  // ---- Item CRUD (server is the source of truth for project state) ----
  // `cartOnly` routes the item into the server document's separate
  // cartOnlyItems array (cart slots), not the playlist tree — keeping the two
  // lists distinct while still registering the cue with the engine.
  async function addProjectItem(item: any, parentUuid: string = '', cartOnly: boolean = false) {
    return rest<any>('/api/project/items', {
      method: 'POST',
      body: JSON.stringify({ item, parentUuid, cartOnly }),
    }).then(p => { fetchCues(); return p; });
  }
  async function updateProjectItem(uuid: string, patch: any) {
    return rest<any>(`/api/project/items/${encodeURIComponent(uuid)}`, {
      method: 'PATCH',
      body: JSON.stringify(patch),
    });
  }
  async function removeProjectItem(uuid: string) {
    return rest<any>(`/api/project/items/${encodeURIComponent(uuid)}`, {
      method: 'DELETE',
    }).then(p => { fetchCues(); return p; });
  }
  async function reorderProjectItems(uuids: string[], parentUuid: string = '') {
    return rest<any>('/api/project/items/reorder', {
      method: 'POST',
      body: JSON.stringify({ uuids, parentUuid }),
    });
  }

  // Transport by item uuid (preferred over cue_id — preserves duckingBehavior
  // and inPoint semantics on the server side). The server routes `play` for
  // a group uuid through trigger_item so group startBehavior fires.
  function playItem(uuid: string)  { wsSend({ type: 'play', item_uuid: uuid }); }
  function stopItem(uuid: string)  { wsSend({ type: 'stop', item_uuid: uuid }); }
  function pauseItem(uuid: string) { wsSend({ type: 'pause',  item_uuid: uuid }); }
  function resumeItem(uuid: string){ wsSend({ type: 'resume', item_uuid: uuid }); }
  // Tell the server which item to play when the currently-playing item's
  // end-behavior fires "next". Pass null to clear.
  function setNextItem(uuid: string | null) {
    wsSend({ type: 'set_next_item', item_uuid: uuid ?? '' });
  }
  // Low-latency seek over the WebSocket so scrub bars feel responsive. The
  // REST endpoint is still available for callers that want a guaranteed
  // ack (mostly tooling) — see seekItemREST below.
  function seekItem(uuid: string, seconds: number) {
    wsSend({ type: 'seek', item_uuid: uuid, seconds });
  }
  function seekCueId(cueId: string, seconds: number) {
    wsSend({ type: 'seek', cue_id: cueId, seconds });
  }
  async function seekItemREST(uuid: string, seconds: number) {
    return rest<any>(`/api/project/items/${encodeURIComponent(uuid)}/seek`, {
      method: 'POST',
      body: JSON.stringify({ seconds }),
    });
  }

  // Cart slot bindings.
  async function setCartSlot(slot: number, itemUuid: string) {
    return rest<any>('/api/project/cart', {
      method: 'POST',
      body: JSON.stringify({ slot, itemUuid }),
    });
  }
  async function clearCartSlot(slot: number) {
    return rest<any>(`/api/project/cart/${slot}`, { method: 'DELETE' });
  }

  // Preview (DJ-style pre-listening on settings.previewDevice).
  async function startPreview(itemUuid: string) {
    return rest<any>('/api/preview', {
      method: 'POST',
      body: JSON.stringify({ itemUuid }),
    });
  }
  async function stopPreview() {
    return rest<any>('/api/preview', { method: 'DELETE' });
  }
  async function fetchPreviewState() {
    return rest<{ active: boolean; itemUuid: string }>('/api/preview');
  }

  // Master gain (dB). Server is the authority — REST POST persists and
  // broadcasts to every client via master_gain_changed doc_patch.
  async function setMasterGainDb(db: number) {
    return rest<any>('/api/master/gain', {
      method: 'POST',
      body: JSON.stringify({ db }),
    });
  }
  async function fetchMasterGainDb() {
    return rest<{ db: number }>('/api/master/gain');
  }

  // Per-output-channel gain. Broadcasts output_channel_gain_changed to all clients.
  async function setOutputChannelGainDb(channel: number, db: number) {
    return rest<any>(`/api/master/channels/${channel}/gain`, {
      method: 'POST',
      body: JSON.stringify({ db }),
    });
  }

  // Reactive map of per-output-channel gains (channel index → dB).
  const outputChannelGains = ref<Record<number, number>>({});

  // Theme + settings shallow-merge patches.
  async function patchTheme(patch: any) {
    return rest<any>('/api/project/theme', {
      method: 'PATCH',
      body: JSON.stringify(patch),
    });
  }
  async function patchSettings(patch: any) {
    return rest<any>('/api/project/settings', {
      method: 'PATCH',
      body: JSON.stringify(patch),
    });
  }
  async function addCueFromPath(filePath: string, displayName?: string) {
    const cue = await rest<ServerCue>('/api/cues', {
      method: 'POST',
      body: JSON.stringify({ file_path: filePath, display_name: displayName }),
    });
    // Refresh the cue list in the background — callers need the new cue id
    // (already returned by POST), they should NOT block on a secondary fetch.
    // Awaiting this was deadlocking play() when the GET stalled.
    fetchCues().catch(() => { /* best-effort */ });
    return cue;
  }
  async function removeCue(cueId: CueId) {
    await rest(`/api/cues/${encodeURIComponent(cueId)}`, { method: 'DELETE' });
    cues.value = cues.value.filter(c => c.id !== cueId);
  }
  async function setCueLtc(cueId: CueId, enabled: boolean, fps: number, offsetNs: number) {
    return rest(`/api/cues/${encodeURIComponent(cueId)}/ltc`, {
      method: 'POST',
      body: JSON.stringify({ enabled, fps, offset_ns: offsetNs }),
    });
  }

  // ---- Routing ------------------------------------------------------
  async function routeItemToMixer(cue: CueId, sourceCh: number,
                                  mixer: MixerChannelId, gainDb = 0) {
    return rest('/api/routing/item_to_mixer', {
      method: 'POST',
      body: JSON.stringify({ cue, source_channel: sourceCh, mixer, gain_db: gainDb }),
    });
  }
  async function routeMixerToMaster(mixer: MixerChannelId,
                                    masterChannel: MasterChannelIndex,
                                    gainDb = 0) {
    return rest('/api/routing/mixer_to_master', {
      method: 'POST',
      body: JSON.stringify({ mixer, master_channel: masterChannel, gain_db: gainDb }),
    });
  }
  async function assignMasterToDevice(masterChannel: MasterChannelIndex,
                                      device: DeviceId, hwChannel: number) {
    return rest('/api/routing/master_to_device', {
      method: 'POST',
      body: JSON.stringify({ master_channel: masterChannel, device, hw_channel: hwChannel }),
    });
  }
  async function createMixerChannel(name: string) {
    const out = await rest<{ id: MixerChannelId }>('/api/mixers', {
      method: 'POST',
      body: JSON.stringify({ name }),
    });
    fetchMixerChannels().catch(() => {});   // fire-and-forget refresh
    return out.id;
  }
  async function removeMixerChannel(id: MixerChannelId) {
    await rest(`/api/mixers/${encodeURIComponent(id)}`, { method: 'DELETE' });
    fetchMixerChannels().catch(() => {});
  }

  // ---- Devices ------------------------------------------------------
  async function openDevice(name = '', channels = 2) {
    const out = await rest<{ device_id: DeviceId }>('/api/devices/open', {
      method: 'POST',
      body: JSON.stringify({ name, channels }),
    });
    fetchDevices().catch(() => {});
    return out.device_id;
  }
  async function closeDevice(id: DeviceId) {
    await rest('/api/devices/close', {
      method: 'POST',
      body: JSON.stringify({ id }),
    });
    fetchDevices().catch(() => {});
  }

  // ---- Filesystem ---------------------------------------------------
  // filter:  'audio' (default), 'all', or a comma list of extensions like
  //          '.liveplay,.lpa'. Server side enforces; client just passes through.
  async function listServerPath(path: string, filter: string = 'audio') {
    const url = '/api/fs/list?path=' + encodeURIComponent(path) +
                '&filter=' + encodeURIComponent(filter);
    return rest<ServerFsListing>(url);
  }
  async function createServerDirectory(path: string) {
    return rest<{ path: string }>('/api/fs/mkdir', {
      method: 'POST',
      body: JSON.stringify({ path }),
    });
  }

  // ---- Upload (multipart) -------------------------------------------
  async function uploadFile(file: File | Blob, filename?: string) {
    const fd = new FormData();
    fd.append('file', file, filename ?? (file as File).name);
    const res = await fetch(httpBase.value + '/api/upload', {
      method: 'POST',
      body: fd,
    });
    if (!res.ok) {
      const text = await res.text().catch(() => res.statusText);
      throw new Error(`upload failed: ${res.status} ${text}`);
    }
    return res.json() as Promise<{ saved: string[] }>;
  }

  // ---- Waveform fetch queue + cache --------------------------------
  // Caps concurrent waveform requests to avoid overwhelming the server
  // when many items enter the viewport simultaneously (e.g. initial mount).
  const WAVEFORM_CONCURRENCY = 3;
  const waveformCache = new Map<string, ServerWaveform>();
  let waveformInFlight = 0;
  const waveformQueue: Array<() => void> = [];

  function drainWaveformQueue() {
    while (waveformInFlight < WAVEFORM_CONCURRENCY && waveformQueue.length > 0) {
      const next = waveformQueue.shift()!;
      next();
    }
  }

  async function fetchWaveform(cueId: CueId, buckets = 1000): Promise<ServerWaveform> {
    const key = `${cueId}:${buckets}`;
    if (waveformCache.has(key)) return waveformCache.get(key)!;

    return new Promise<ServerWaveform>((resolve, reject) => {
      const execute = async () => {
        waveformInFlight++;
        try {
          const data = await rest<ServerWaveform>(
            `/api/waveform/${encodeURIComponent(cueId)}?buckets=${buckets}`);
          waveformCache.set(key, data);
          resolve(data);
        } catch (e) {
          reject(e);
        } finally {
          waveformInFlight--;
          drainWaveformQueue();
        }
      };
      if (waveformInFlight < WAVEFORM_CONCURRENCY) {
        execute();
      } else {
        waveformQueue.push(execute);
      }
    });
  }

  function invalidateWaveformCache(cueId?: CueId) {
    if (cueId) {
      for (const key of waveformCache.keys()) {
        if (key.startsWith(`${cueId}:`)) waveformCache.delete(key);
      }
    } else {
      waveformCache.clear();
    }
  }
  async function fetchMetadata(path: string) {
    return rest('/api/metadata?path=' + encodeURIComponent(path));
  }

  // Compute a waveform for an arbitrary server-side file path. Used right
  // after import so the client can show a waveform without the file needing
  // to be a registered engine cue yet.
  async function fetchWaveformByPath(
    filePath: string,
    buckets = 1000,
  ): Promise<ServerWaveform> {
    const params = new URLSearchParams({ path: filePath, buckets: String(buckets) });
    return rest<ServerWaveform>(`/api/waveform_path?${params}`);
  }

  // Copy a server-side file into the project's media root. Returns the
  // absolute path of the copy. A no-op (returns the same path) if the file
  // is already inside the media root.
  async function copyToMedia(sourcePath: string): Promise<string> {
    const result = await rest<{ dest_path: string }>('/api/copy_to_media', {
      method: 'POST',
      body: JSON.stringify({ source_path: sourcePath }),
    });
    return result.dest_path;
  }

  // Land a file dropped from the OS into the server's media root and return
  // the absolute server path it now lives at (or null on failure).
  //
  // Two transports, picked automatically so this works regardless of where the
  // server runs:
  //   * Local server — when Electron can give us the dropped file's OS path and
  //     the server shares this filesystem, ask the server to copy it in place
  //     (no byte transfer over the wire).
  //   * Remote server / browser / no path — upload the bytes via multipart.
  // copyToMedia failing (e.g. a remote server that can't see the path) falls
  // back to upload, so a misdetected "local" server still works.
  async function resolveDroppedFileToMedia(file: File): Promise<string | null> {
    const osPath = (import.meta.client && (window as any).electronAPI?.getFilePath)
      ? (window as any).electronAPI.getFilePath(file)
      : null;
    if (osPath) {
      try { return await copyToMedia(osPath); }
      catch (e) { console.warn('[import] copyToMedia failed, uploading bytes instead:', e); }
    }
    try {
      const res = await uploadFile(file, file.name);
      return res?.saved?.[0] ?? null;
    } catch (e) {
      console.error('[import] upload failed:', e);
      return null;
    }
  }

  // Queue an async waveform generation on the server. Returns immediately;
  // the result arrives as a { op: 'waveform_ready', item_uuid, channels, ... }
  // doc_patch over WebSocket once computation finishes.
  // Pass force=true to delete any cached waveform file and recompute from scratch.
  async function requestWaveformGeneration(path: string, itemUuid: string, force = false): Promise<void> {
    await rest('/api/waveform_generate', {
      method: 'POST',
      body: JSON.stringify({ path, item_uuid: itemUuid, force }),
    });
  }

  // ---- Cleanup ------------------------------------------------------
  function destroy() {
    disconnect();
  }

  return reactive({
    // state
    serverUrl,
    connected,
    reconnecting,
    connectionLost,
    lastError,
    cues,
    mixerChannels,
    devices,
    meters,

    // config
    setServerUrl,

    // lifecycle
    connect,
    disconnect,
    forceReconnect,
    destroy,
    onMeters,
    onCueState,
    onDocPatch,
    onPlaybackSnapshot,

    // transport
    play,
    stop,
    stopAll,
    setGainDb,
    setFade,
    setCueLtc,
    ping,

    // catalogue
    fetchCues,
    fetchMixerChannels,
    fetchDevices,
    addCueFromPath,
    removeCue,
    createMixerChannel,
    removeMixerChannel,

    // routing
    routeItemToMixer,
    routeMixerToMaster,
    assignMasterToDevice,

    // devices
    openDevice,
    closeDevice,

    // fs / uploads / waveform
    listServerPath,
    createServerDirectory,
    uploadFile,
    fetchWaveform,
    fetchWaveformByPath,
    copyToMedia,
    resolveDroppedFileToMedia,
    requestWaveformGeneration,
    invalidateWaveformCache,
    fetchMetadata,

    // project I/O
    fetchProject,
    fetchProjectHeader,
    fetchProjectItemsPage,
    fetchProjectProgress,
    loadProjectFromPath,
    loadProjectFromDocument,
    replaceProjectDocument,
    saveProjectTo,
    repairProject,
    closeProjectOnServer,
    isLocalServer,
    refreshIsLocalServer,
    exportProjectArchive,
    downloadArchive,
    importProjectArchiveUpload,
    importProjectArchiveFromServer,

    // item CRUD via server
    addProjectItem,
    updateProjectItem,
    removeProjectItem,
    reorderProjectItems,

    // transport by item uuid
    playItem,
    stopItem,
    pauseItem,
    resumeItem,
    setNextItem,
    seekItem,
    seekCueId,
    seekItemREST,
    setMasterGainDb,
    fetchMasterGainDb,
    outputChannelGains,
    setOutputChannelGainDb,

    // cart bindings
    setCartSlot,
    clearCartSlot,

    // theme + settings
    patchTheme,
    patchSettings,

    // preview
    startPreview,
    stopPreview,
    fetchPreviewState,
  });
}

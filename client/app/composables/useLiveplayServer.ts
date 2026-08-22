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
import type { Bus, BusDsp } from '~/types/project';
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

/** GET /api/outputs: the machine's logical-output map, plus the built-in names. */
export interface OutputMapChannel { device: string; hwChannel: number }
export interface OutputMapEntry   { name: string; channels: OutputMapChannel[] }
export interface OutputMap {
  version: number;
  outputs: OutputMapEntry[];
  builtin?: string[];
}

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
    master_bus?: { channels: number; preview_l: number; preview_r: number };
  };
  type PlaybackSnapshotSubscriber = (s: PlaybackSnapshot) => void;
  const playbackSnapshotSubscribers = new Set<PlaybackSnapshotSubscriber>();
  function onPlaybackSnapshot(cb: PlaybackSnapshotSubscriber): () => void {
    playbackSnapshotSubscribers.add(cb);
    return () => playbackSnapshotSubscribers.delete(cb);
  }

  // Subscribers notified when the socket comes back *after* the connection was
  // declared lost. Distinct from a plain `connected` watch, which also fires on
  // the first connect of the session.
  type ReconnectedSubscriber = () => void;
  const reconnectedSubscribers = new Set<ReconnectedSubscriber>();
  function onReconnected(cb: ReconnectedSubscriber): () => void {
    reconnectedSubscribers.add(cb);
    return () => reconnectedSubscribers.delete(cb);
  }

  // Subscribers for multi-client doc_patch fan-out events.
  // Payload: { type: 'doc_patch', op: 'item_added'|'item_updated'|... , ... }
  type DocPatchSubscriber = (p: any) => void;
  const docPatchSubscribers = new Set<DocPatchSubscriber>();
  function onDocPatch(cb: DocPatchSubscriber): () => void {
    docPatchSubscribers.add(cb);
    return () => docPatchSubscribers.delete(cb);
  }

  // D12 — legacy-project migration banner. Pure client view state: the server
  // only tells us a migration *happened* (once, via the doc_patch broadcast
  // below); whether the banner is still showing in this window is local, so
  // it can be dismissed independently per client (D17 requires every
  // connected window to see it fire, not that they agree on dismissal).
  // null = no banner to show. Set fresh on every project_migrated broadcast
  // (even if a previous one was dismissed) so a later migration re-alerts.
  type MigrationBannerState = {
    itemsToMain: number;
    busesFromDeviceOverride: number;
    mainOutputMigrated: boolean;
  };
  const migrationBanner = ref<MigrationBannerState | null>(null);
  function dismissMigrationBanner() {
    migrationBanner.value = null;
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
  // Informational only (surfaced in the modal) — the "we're really down"
  // decision is time-based, see below.
  const failedReconnectAttempts = ref(0);
  // True once we've decided the server is gone. Cleared on a successful
  // reconnect. UI binds to this to show the connection-lost modal and to
  // freeze every control that would otherwise mutate unreachable state.
  const connectionLost = ref(false);
  // Grace period before we shout about it. A WS bounce that heals inside this
  // window is invisible to the operator; anything longer gets the modal.
  // Time-based rather than attempt-based so the delay is predictable no matter
  // where the exponential backoff happens to be.
  const CONNECTION_LOST_DELAY_MS = 3000;
  let connectionLostTimer: ReturnType<typeof setTimeout> | null = null;

  function armConnectionLostTimer() {
    if (connectionLostTimer || connectionLost.value) return;
    connectionLostTimer = setTimeout(() => {
      connectionLostTimer = null;
      if (!connected.value) connectionLost.value = true;
    }, CONNECTION_LOST_DELAY_MS);
  }

  function clearConnectionLostTimer() {
    if (connectionLostTimer) {
      clearTimeout(connectionLostTimer);
      connectionLostTimer = null;
    }
  }

  function scheduleReconnect() {
    if (reconnectTimer) return;
    reconnecting.value = true;
    reconnectTimer = setTimeout(() => {
      reconnectTimer = null;
      reconnectDelay = Math.min(reconnectDelay * 2, 10000);
      connect();
    }, reconnectDelay);
  }

  // Force an immediate reconnect attempt, resetting the backoff so the retry
  // happens now instead of up to 10 s from now. Used by the Reconnect button.
  // Deliberately does NOT clear `connectionLost`: the modal stays up until we
  // are actually back, otherwise a failed retry would flash the UI open and
  // then re-lock it a moment later.
  function forceReconnect() {
    if (reconnectTimer) { clearTimeout(reconnectTimer); reconnectTimer = null; }
    reconnectDelay = 1500;
    failedReconnectAttempts.value = 0;
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
      clearConnectionLostTimer();
      const wasLost = connectionLost.value;
      connectionLost.value = false;
      // Fires only on a recovery, never on the very first connect. Subscribers
      // (useConnectionGuard) use this to verify the server still holds the
      // session we think it does — a restarted server accepts the socket but
      // has forgotten the project.
      if (wasLost) {
        for (const cb of reconnectedSubscribers) {
          try { cb(); } catch (e) { console.warn('[liveplay] reconnect handler threw:', e); }
        }
      }
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
        void Promise.allSettled([fetchCues(), fetchMixerChannels(), fetchDevices(), fetchBuses()]);
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
      // Count pre-handshake closes — the socket bounced straight to close
      // without an onopen in between. Purely for display in the modal.
      if (!wasConnected && hasEverConnected) failedReconnectAttempts.value++;
      // Start (or keep) the grace-period countdown to the "connection lost"
      // lockout. Only meaningful once we've had a connection to lose: at cold
      // boot the welcome screen is the right place to notice a dead server,
      // not a modal over an empty app.
      if (hasEverConnected) armConnectionLostTimer();
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
          // Adopt the server's master-bus geometry. Absent on older servers,
          // in which case the 32-wide defaults stay in place.
          if (snap.master_bus) {
            masterBus.value = {
              channels:  snap.master_bus.channels,
              previewL:  snap.master_bus.preview_l,
              previewR:  snap.master_bus.preview_r,
            };
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
          // Another client changed the mixer — refetch so this one converges.
          // The payload carries the definitions, but not the resolved item
          // membership, which only the server can compute.
          if (payload.op === 'buses_patched') void fetchBuses();
          // A remapped output can silently change `bound` on any bus whose
          // output targets it (D10 — bound is server-computed; the server's
          // PUT /api/outputs rewires affected buses but does not also emit
          // buses_patched). Refetch so a strip's warn state clears without
          // needing a reload.
          if (payload.op === 'outputs_changed') void fetchBuses();
          // D12/D17 — a legacy project (pre-mixer-buses) was just migrated on
          // load. The server sends this once, to every connected client, only
          // when something actually migrated (a reload of an already-migrated
          // doc broadcasts nothing) — so this fires for windows that didn't
          // even issue the load that triggered it. Counts arrive flat on the
          // frame, not nested under a `migration` key.
          if (payload.op === 'project_migrated') {
            migrationBanner.value = {
              itemsToMain: Number(payload.itemsToMain) || 0,
              busesFromDeviceOverride: Number(payload.busesFromDeviceOverride) || 0,
              mainOutputMigrated: !!payload.mainOutputMigrated,
            };
            void fetchBuses();
          }
          // PFL isn't in the document, so it arrives as its own op and is
          // applied in place. A refetch would work too, but PFL is pressed
          // while something is playing and the whole bus list is the last
          // thing worth re-pulling at that moment.
          if (payload.op === 'bus_pfl_changed' && typeof payload.id === 'string') {
            const b = buses.value.find(x => x.id === payload.id);
            if (b) b.pfl = !!payload.pfl;
          }
          if (payload.op === 'bus_pfl_cleared') {
            for (const b of buses.value) b.pfl = false;
          }
          // Same story as PFL: live monitoring state, its own op, applied in
          // place so a second mixer window agrees about what the phones are
          // doing. It lands on whichever bus holds the preview role (D33),
          // not on a fixed id — the role can move.
          if (payload.op === 'monitor_mono_changed') {
            const mon = buses.value.find(x => x.preview);
            if (mon) mon.monoCheck = !!payload.mono;
          }
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
    // An intentional teardown isn't a lost connection. Any in-flight grace
    // period is void; a genuine failure after this re-arms it from onclose.
    clearConnectionLostTimer();
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
  // Omit fadeMs to let the server apply the project-wide Stop All fade
  // (settings.stopAllFadeMs, default 1000 ms). Pass a number (incl. 0 for an
  // instant panic) to override it for this call.
  function stopAll(fadeMs?: number) {
    wsSend(fadeMs === undefined
      ? { type: 'stop_all' }
      : { type: 'stop_all', fade_ms: fadeMs });
  }
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

  // ---- Shared operator UI state --------------------------------------
  // Selection, Show Mode and the display locale live on the server so every
  // client and control surface (Bitfocus Companion) agrees on them. These
  // senders are fire-and-forget: the server echoes the change back as a
  // doc_patch, and that echo — not the local call — is what updates state.
  function setSelection(uuid: string | null) {
    wsSend({ type: 'set_selection', item_uuid: uuid ?? '' });
  }
  function stepSelection(delta: number) {
    wsSend({ type: 'select_step', delta });
  }
  // Omit `enabled` to toggle.
  function setShowMode(enabled?: boolean) {
    wsSend(enabled === undefined
      ? { type: 'set_show_mode' }
      : { type: 'set_show_mode', enabled });
  }
  function setServerLocale(locale: string) {
    wsSend({ type: 'set_locale', locale });
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

  // Preview (DJ-style pre-listening, rendered on the Preview bus).
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

  // Master-bus geometry, learned from the server's playback_snapshot. The bus
  // width is configurable at boot, so the preview pair is not always 30/31 —
  // the UI must place output meters from these values rather than assume. The
  // defaults below match a 32-wide bus so a pre-#5 server still renders right.
  const masterBus = ref<{ channels: number; previewL: number; previewR: number }>({
    channels: 32,
    previewL: 30,
    previewR: 31,
  });

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

  // Live strip level / mute. These hit the engine only — no document write, no
  // refetch — so they are cheap enough to call while a fader is moving. The
  // owning bus is persisted separately once the gesture settles.
  async function setMixerGainDb(mixerId: MixerChannelId, db: number) {
    return rest(`/api/mixers/${encodeURIComponent(mixerId)}/gain`, {
      method: 'POST',
      body: JSON.stringify({ db }),
    });
  }
  async function setMixerMute(mixerId: MixerChannelId, muted: boolean) {
    return rest(`/api/mixers/${encodeURIComponent(mixerId)}/mute`, {
      method: 'POST',
      body: JSON.stringify({ muted }),
    });
  }

  // ---- Buses --------------------------------------------------------
  // The user-facing mixer. Every mutation is authoritative on the server, so
  // the local list is refreshed from it rather than patched optimistically —
  // creating a bus can be refused at the strip limit, and deleting one
  // reassigns items, neither of which the client can predict.
  const buses = ref<Bus[]>([]);

  async function fetchBuses() {
    try {
      buses.value = await rest<Bus[]>('/api/buses');
    } catch { /* offline; the WS reconnect path refetches */ }
    return buses.value;
  }

  async function createBus(spec: Partial<Bus> & { name: string }) {
    const out = await rest<{ id: string }>('/api/buses', {
      method: 'POST',
      body: JSON.stringify(spec),
    });
    await fetchBuses();
    return out.id;
  }

  async function patchBus(id: string, patch: Partial<Bus>) {
    await rest(`/api/buses/${encodeURIComponent(id)}`, {
      method: 'PATCH',
      body: JSON.stringify(patch),
    });
    await fetchBuses();
  }

  // Live pan for the duration of a knob drag. Moves the send gains in the
  // engine only — no document write, no refetch — exactly as setMixerGainDb
  // does for the fader. The caller PATCHes the settled value.
  async function setBusPan(id: string, pan: number) {
    return rest(`/api/buses/${encodeURIComponent(id)}/pan`, {
      method: 'POST',
      body: JSON.stringify({ pan }),
    });
  }

  // Pre-fade listen. The button is set locally first because the operator is
  // holding it against a cue that is playing right now — waiting a round trip
  // to light up reads as a dropped press. The server broadcasts the change, so
  // every other window converges on the same value anyway.
  async function setBusPfl(id: string, on: boolean) {
    const b = buses.value.find(x => x.id === id);
    if (b) b.pfl = on;
    try {
      await rest(`/api/buses/${encodeURIComponent(id)}/pfl`, {
        method: 'POST',
        body: JSON.stringify({ pfl: on }),
      });
    } catch (e) {
      // Refused (the preview-role bus is the destination, not a source) or
      // offline: put the button back where the server still has it rather
      // than leaving a lie lit.
      if (b) b.pfl = !on;
      throw e;
    }
  }

  async function clearAllPfl() {
    for (const b of buses.value) b.pfl = false;
    return rest<{ cleared: number }>('/api/buses/pfl/clear', { method: 'POST' });
  }

  // Fold the preview-role bus to mono, to check what is in the phones for
  // mono compatibility. Set locally first for the same reason PFL is: this is
  // a press made against something playing right now.
  //
  // Not addressed per bus — it is one control for the whole monitoring path,
  // and the server applies it to whichever bus currently holds the preview
  // role (D33; /api/preview/mono is the same endpoint under its new name).
  async function setMonitorMono(on: boolean) {
    const mon = buses.value.find(x => x.preview);
    if (mon) mon.monoCheck = on;
    try {
      await rest('/api/monitor/mono', {
        method: 'POST',
        body: JSON.stringify({ mono: on }),
      });
    } catch (e) {
      if (mon) mon.monoCheck = !on;
      throw e;
    }
  }

  // Live tone controls for the duration of a filter drag. Coefficients go
  // straight to the strip — no document write, no refetch — exactly as
  // setBusPan does, and for the same reason: a PATCH per drag event would
  // rewrite the document and bounce the knob to the stale value.
  async function setBusDsp(id: string, dsp: Partial<BusDsp>) {
    return rest(`/api/buses/${encodeURIComponent(id)}/dsp`, {
      method: 'POST',
      body: JSON.stringify(dsp),
    });
  }

  async function deleteBus(id: string) {
    await rest(`/api/buses/${encodeURIComponent(id)}`, { method: 'DELETE' });
    await fetchBuses();
  }

  // Move a role (D24) onto this bus. The server does it atomically — the
  // previous holder loses the flag, both are rewired, one broadcast — and it
  // is the only way a role moves: {master:false} is refused, because a
  // project always has exactly one of each. Refetched rather than patched
  // locally since two buses change, and the rewire can change `masters` and
  // `bound` on both.
  async function setBusRole(id: string, role: 'master' | 'preview') {
    await rest(`/api/buses/${encodeURIComponent(id)}`, {
      method: 'PATCH',
      body: JSON.stringify({ [role]: true }),
    });
    await fetchBuses();
  }

  // Rail position. The server stores the number and broadcasts; the rail is
  // whatever sorting by `order` gives, so the caller picks a value that lands
  // the bus where it was dropped (MixerPanel.vue decides how).
  async function reorderBus(id: string, order: number) {
    await rest(`/api/buses/${encodeURIComponent(id)}`, {
      method: 'PATCH',
      body: JSON.stringify({ order }),
    });
    await fetchBuses();
  }

  // Assign an item (or group) to a bus. Passing null clears the assignment so
  // it inherits from its group, or falls back to the master-role bus.
  async function setItemBus(uuid: string, busId: string | null) {
    await rest(`/api/project/items/${encodeURIComponent(uuid)}`, {
      method: 'PATCH',
      body: JSON.stringify({ busId }),
    });
    // Membership is resolved server-side (an item can inherit its bus from a
    // group), so refetch rather than guessing which bus it landed on.
    await fetchBuses();
  }

  // ---- Logical outputs (server-owned; never in the project) ----------
  // `builtin` (D26) names the outputs the server special-cases when they
  // have no row: "Main Out" falls back to the default device, "Preview Out"
  // stays silent. They are offered and mapped like any other name.
  async function fetchOutputs() {
    return rest<OutputMap>('/api/outputs');
  }
  async function saveOutputs(map: unknown) {
    return rest('/api/outputs', { method: 'PUT', body: JSON.stringify(map) });
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
    failedReconnectAttempts,
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
    onReconnected,
    migrationBanner,
    dismissMigrationBanner,

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

    // shared operator UI state (server-owned; mirrored to every client)
    setSelection,
    stepSelection,
    setShowMode,
    setServerLocale,

    seekItem,
    seekCueId,
    seekItemREST,
    setMasterGainDb,
    fetchMasterGainDb,
    outputChannelGains,
    setOutputChannelGainDb,
    masterBus,

    setMixerGainDb,
    setMixerMute,

    // buses
    buses,
    fetchBuses,
    createBus,
    patchBus,
    setBusPan,
    setBusDsp,
    setBusPfl,
    clearAllPfl,
    setMonitorMono,
    deleteBus,
    setBusRole,
    reorderBus,
    setItemBus,
    fetchOutputs,
    saveOutputs,

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

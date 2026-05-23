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

  // ---- WebSocket ----------------------------------------------------
  let ws: WebSocket | null = null;
  let reconnectTimer: ReturnType<typeof setTimeout> | null = null;
  let reconnectDelay = 1500;         // start higher; backs off to 10 s
  // True after the *very first* successful onopen for this session. Used to
  // skip the expensive triple-fetch (cues, mixers, devices) on every reconnect
  // — those don't change just because the WS bounced.
  let hasEverConnected = false;

  function scheduleReconnect() {
    if (reconnectTimer) return;
    reconnecting.value = true;
    reconnectTimer = setTimeout(() => {
      reconnectTimer = null;
      reconnectDelay = Math.min(reconnectDelay * 2, 10000);
      connect();
    }, reconnectDelay);
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
      // Only refresh REST-backed catalogues on the very first connection.
      // Reconnects (e.g. transient Crow WS close-frame issues) don't change
      // those tables, so re-fetching every time produced a request storm
      // that masked any actual UI work.
      if (!hasEverConnected) {
        hasEverConnected = true;
        void Promise.allSettled([fetchCues(), fetchMixerChannels(), fetchDevices()]);
      }
    };

    ws.onclose = () => {
      connected.value = false;
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
        case 'cue_state': {
          // Patch local cue cache to reflect immediate transport changes.
          const idx = cues.value.findIndex(c => c.id === payload.cue_id);
          if (idx >= 0) {
            cues.value[idx] = {
              ...cues.value[idx],
              transport: payload.transport,
              playhead_seconds: payload.playhead_seconds,
            };
          }
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
  async function saveProjectTo(path?: string) {
    return rest<any>('/api/project/save', {
      method: 'POST',
      body: JSON.stringify(path ? { path } : {}),
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
  async function addProjectItem(item: any, parentUuid: string = '') {
    return rest<any>('/api/project/items', {
      method: 'POST',
      body: JSON.stringify({ item, parentUuid }),
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
  // and inPoint semantics on the server side).
  function playItem(uuid: string)  { wsSend({ type: 'play', item_uuid: uuid }); }
  function stopItem(uuid: string)  { wsSend({ type: 'stop', item_uuid: uuid }); }
  async function seekItem(uuid: string, seconds: number) {
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

  // ---- Waveform / metadata -----------------------------------------
  async function fetchWaveform(cueId: CueId, buckets = 1000) {
    return rest<ServerWaveform>(
      `/api/waveform/${encodeURIComponent(cueId)}?buckets=${buckets}`);
  }
  async function fetchMetadata(path: string) {
    return rest('/api/metadata?path=' + encodeURIComponent(path));
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
    destroy,
    onMeters,

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
    uploadFile,
    fetchWaveform,
    fetchMetadata,

    // project I/O
    fetchProject,
    fetchProjectProgress,
    loadProjectFromPath,
    loadProjectFromDocument,
    replaceProjectDocument,
    saveProjectTo,

    // item CRUD via server
    addProjectItem,
    updateProjectItem,
    removeProjectItem,
    reorderProjectItems,

    // transport by item uuid
    playItem,
    stopItem,
    seekItem,

    // cart bindings
    setCartSlot,
    clearCartSlot,

    // theme + settings
    patchTheme,
    patchSettings,
  });
}

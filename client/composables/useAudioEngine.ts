// =====================================================================
// useAudioEngine.ts
// ---------------------------------------------------------------------
// Thin projection of the server's playback state. Nothing in here owns
// transport, ducking, fades, end-behaviour, group sequencing or
// custom-action scheduling — that all lives in the C++ ProjectState +
// AudioEngine + sequencer. The composable's only job is:
//
//   1. Translate component-level intents (playCue / stopCue / seekCue /
//      pauseCue / setMasterGain / setNextItem / ...) into WS or REST
//      calls.
//   2. Maintain a *reactive view* of what the server is doing:
//        - `activeCues`    : Map<itemUuid, ActiveCueView>
//        - `activeGroups`  : Map<groupUuid, ActiveGroupView>     (derived)
//        - `masterOutputLevel`, `masterPeakLevel`  (read from server meters)
//        - `masterGainDb`              (server-authoritative)
//        - `nextItemOverrideUuid`      (server-authoritative)
//        - `autoNextItemUuid`          (derived from project doc)
//
// Every field is updated from one of:
//   * `cue_state` WS edges (Stopped/Playing/FadingIn/FadingOut/Paused)
//   * `playback_snapshot` WS message on (re)connect
//   * `meters` WS broadcast (playhead per active cue, master levels)
//   * `doc_patch` ops broadcast from REST mutators (next_item_set,
//     master_gain_changed, preview_started/stopped)
//
// Component compatibility: the public surface (function names + the
// shape of activeCues / activeGroups entries) matches the legacy
// composable so existing components don't need changes.
// =====================================================================
import type { AudioItem, GroupItem, BaseItem } from '~/types/project';

// ---------------------------------------------------------------------
// Shapes consumed by Vue components.
// ---------------------------------------------------------------------
export interface ActiveCueView {
  uuid: string;
  displayName: string;
  duration: number;       // trimmed (outPoint - inPoint)
  currentTime: number;    // playhead relative to inPoint
  isPaused: boolean;
  color?: string;
  inPoint?: number;
  outPoint?: number;
  serverCueId?: string | null;
}

export interface ActiveGroupView {
  uuid: string;
  displayName: string;
  totalDuration: number;
  currentTime: number;
}

// Server's TransportState enum (mirrors C++).
const TRANSPORT_STOPPED   = 0;
const TRANSPORT_PLAYING   = 1;
const TRANSPORT_FADING_IN = 2;
const TRANSPORT_FADING_OUT= 3;
const TRANSPORT_PAUSED    = 4;

export const useAudioEngine = () => {
  const { currentProject, findItemByUuid, findItemByIndex } = useProject();
  const { cartOnlyItems } = useCartItems();
  const server = useLiveplayServer();

  // ---- Reactive state ------------------------------------------------
  const activeCues   = useState<Map<string, ActiveCueView>>('activeCues',  () => new Map());
  const activeGroups = useState<Map<string, ActiveGroupView>>('activeGroups', () => new Map());

  // Master meter values read straight off the server's master_channels
  // broadcast (the engine's real master meter). Combines L+R so the
  // existing master-level UI doesn't need a stereo split.
  const masterOutputLevel = useState<number>('masterOutputLevel', () => -60);
  const masterPeakLevel   = useState<number>('masterPeakLevel',   () => -60);

  // Server-authoritative master gain. setMasterGain pushes via REST;
  // the master_gain_changed doc_patch reflects it back to every client.
  const masterGainDb = useState<number>('masterGainDb', () => 0);

  // Server-authoritative "Up Next" override. setNextItem writes via WS,
  // server fans out as next_item_set doc_patch.
  const nextItemOverrideUuid = useState<string | null>('nextItemOverrideUuid', () => null);

  // ---- Helpers -------------------------------------------------------
  const findItemByServerCueId = (cueId: string): AudioItem | null => {
    if (!currentProject.value || !cueId) return null;
    const walk = (arr: (AudioItem | GroupItem)[]): AudioItem | null => {
      for (const it of arr) {
        if (it.type === 'audio' && (it as any).cueId === cueId) return it as AudioItem;
        if (it.type === 'group') {
          const hit = walk((it as GroupItem).children);
          if (hit) return hit;
        }
      }
      return null;
    };
    const fromPlaylist = walk(currentProject.value.items);
    if (fromPlaylist) return fromPlaylist;
    // Cart-only items live outside the main playlist tree but are still
    // valid playback targets — the server annotates them with cueId in
    // header_document() too. Without this branch their cue_state events
    // had nowhere to land and the UI never showed them as playing.
    for (const it of cartOnlyItems.value.values()) {
      if (it && (it as any).cueId === cueId) return it as AudioItem;
    }
    return null;
  };

  const findParentGroup = (itemUuid: string): GroupItem | null => {
    if (!currentProject.value) return null;
    const search = (group: GroupItem): GroupItem | null => {
      for (const child of group.children) {
        if (child.uuid === itemUuid) return group;
        if (child.type === 'group') {
          const found = search(child as GroupItem);
          if (found) return found;
        }
      }
      return null;
    };
    for (const item of currentProject.value.items) {
      if (item.type === 'group') {
        const found = search(item as GroupItem);
        if (found) return found;
      }
    }
    return null;
  };

  // Build the trimmed (in→out) duration for an audio item.
  const trimmedDuration = (item: AudioItem): number => {
    const inP  = item.inPoint  || 0;
    const outP = item.outPoint || item.duration || 0;
    return Math.max(0, outP - inP);
  };

  // Insert / update an activeCue from a server signal. Idempotent.
  const upsertActiveCue = (item: AudioItem, transport: number,
                           playheadSeconds: number, cueId: string | null) => {
    const inPoint  = item.inPoint  || 0;
    const existing = activeCues.value.get(item.uuid);
    const view: ActiveCueView = existing ? { ...existing } : {
      uuid: item.uuid,
      displayName: item.displayName,
      duration: trimmedDuration(item),
      currentTime: 0,
      isPaused: false,
      color: item.color,
      inPoint: item.inPoint,
      outPoint: item.outPoint,
      serverCueId: cueId,
    };
    view.displayName = item.displayName;
    view.duration    = trimmedDuration(item);
    view.color       = item.color;
    view.inPoint     = item.inPoint;
    view.outPoint    = item.outPoint;
    if (cueId) view.serverCueId = cueId;
    view.currentTime = Math.max(0, Math.min(view.duration, playheadSeconds - inPoint));
    view.isPaused    = (transport === TRANSPORT_PAUSED);
    // Map mutation: explicit set keeps reactivity in a Map<>.
    activeCues.value.set(item.uuid, view);
  };

  const removeActiveCue = (uuid: string) => {
    activeCues.value.delete(uuid);
  };

  // ---- activeGroups projection --------------------------------------
  // A group is "playing" if any of its descendant audio items is in
  // activeCues. totalDuration / currentTime are computed from the
  // group's children list and the playhead of the currently-playing
  // child (server is the timing authority).
  const recomputeActiveGroups = () => {
    if (!currentProject.value) {
      activeGroups.value.clear();
      return;
    }
    const next = new Map<string, ActiveGroupView>();
    const walk = (group: GroupItem) => {
      let total = 0;
      let acc   = 0;
      let foundActive = false;
      let activeReached = false;
      for (const child of group.children) {
        if (child.type === 'audio') {
          const a = child as AudioItem;
          const d = trimmedDuration(a);
          total += d;
          const playing = activeCues.value.get(a.uuid);
          if (playing) {
            foundActive = true;
            acc += playing.currentTime;
            activeReached = true;
          } else if (!activeReached) {
            acc += d;     // assume earlier siblings already played
          }
        } else if (child.type === 'group') {
          walk(child as GroupItem);  // nested groups get their own entry
        }
      }
      if (foundActive) {
        // Reset acc-of-earlier-siblings heuristic if we never actually
        // found a currently-playing child here.
      }
      if (foundActive) {
        next.set(group.uuid, {
          uuid: group.uuid,
          displayName: group.displayName,
          totalDuration: total,
          currentTime: Math.min(acc, total),
        });
      }
    };
    for (const item of currentProject.value.items) {
      if (item.type === 'group') walk(item as GroupItem);
    }
    activeGroups.value = next;
  };

  // ---- autoNextItemUuid (derived) -----------------------------------
  const autoNextItemUuid = computed((): string | null => {
    if (!currentProject.value) return null;
    for (const [uuid] of activeCues.value.entries()) {
      const item = findItemByUuid(uuid);
      if (!item || item.type !== 'audio') continue;
      const audioItem = item as AudioItem;
      switch (audioItem.endBehavior.action) {
        case 'next': {
          const nextIndex = [...audioItem.index];
          nextIndex[nextIndex.length - 1]++;
          const nextItem = findItemByIndex(nextIndex);
          if (nextItem) return nextItem.uuid;
          break;
        }
        case 'goto-item':
          if (audioItem.endBehavior.targetUuid) return audioItem.endBehavior.targetUuid;
          break;
        case 'goto-index':
          if (audioItem.endBehavior.targetIndex) {
            const target = findItemByIndex(audioItem.endBehavior.targetIndex);
            if (target) return target.uuid;
          }
          break;
      }
    }
    return null;
  });

  // ---- WS / REST plumbing -------------------------------------------
  // cue_state edges: Playing/FadingIn/Paused create or update; Stopped
  // removes. FadingOut keeps the entry so the bar continues to render
  // its trailing seconds.
  server.onCueState(({ cue_id, transport, playhead_seconds, item_uuid }: any) => {
    if (transport === TRANSPORT_STOPPED) {
      for (const [uuid, cue] of activeCues.value) {
        if (cue.serverCueId === cue_id) {
          removeActiveCue(uuid);
          break;
        }
      }
      recomputeActiveGroups();
      return;
    }
    // Prefer item_uuid (server now includes it) so cart items without a
    // cueId annotation are still resolved. Fall back to cueId lookup.
    const item = (item_uuid ? findItemByUuid(item_uuid) : null) ?? findItemByServerCueId(cue_id);
    if (!item || item.type !== 'audio') return;
    upsertActiveCue(item as AudioItem, transport, playhead_seconds, cue_id);
    recomputeActiveGroups();
  });

  // On (re)connect the server pushes a snapshot of what's already
  // playing. useLiveplayServer fires this BEFORE synthesising per-cue
  // events; we use it to refresh master gain + next-item state too.
  server.onPlaybackSnapshot((snap: any) => {
    if (snap && typeof snap.master_gain_db === 'number') {
      masterGainDb.value = snap.master_gain_db;
    }
    nextItemOverrideUuid.value = snap?.next_item_uuid || null;
  });

  // Doc_patch: server-driven state changes other clients might trigger.
  server.onDocPatch((patch: any) => {
    if (!patch || typeof patch !== 'object') return;
    switch (patch.op) {
      case 'next_item_set':
        nextItemOverrideUuid.value = patch.itemUuid || null;
        break;
      case 'master_gain_changed':
        if (typeof patch.db === 'number') masterGainDb.value = patch.db;
        break;
      case 'custom_action_http':
        // The server's sequencer fired a custom http-request action.
        // We execute it client-side because the server has no HTTP
        // client of its own. Best-effort; failures are logged.
        void executeHttpRequest(patch.action?.request);
        break;
    }
  });

  // Meter broadcast: master levels + per-cue playhead. Master level is
  // a derived L+R sum so the existing single-bar UI keeps working;
  // accurate per-channel meters use StereoMeter directly.
  server.onMeters((m: any) => {
    if (!m) return;
    if (Array.isArray(m.items)) {
      for (const meter of m.items) {
        for (const cue of activeCues.value.values()) {
          if (cue.serverCueId !== meter.cue_id) continue;
          const inP = cue.inPoint || 0;
          cue.currentTime = Math.max(0,
            Math.min(meter.playhead_seconds - inP, cue.duration));
          break;
        }
      }
      // Rebuild group projection so playhead changes propagate to
      // group progress indicators.
      recomputeActiveGroups();
    }
    if (Array.isArray(m.master_channels) && m.master_channels.length > 0) {
      // Sum L and R (channels 0 and 1) for a single mono master level.
      let peakDb = -60;
      let combinedRms = 0;
      for (const mc of m.master_channels) {
        if (mc.index !== 0 && mc.index !== 1) continue;
        if (typeof mc.peak_db === 'number' && mc.peak_db > peakDb) peakDb = mc.peak_db;
        if (typeof mc.rms_db === 'number') {
          // Convert dB → linear and sum so 2× the level reads ~+6 dB.
          combinedRms += Math.pow(10, mc.rms_db / 20);
        }
      }
      const rmsDb = combinedRms > 0
        ? Math.max(-60, 20 * Math.log10(combinedRms))
        : -60;
      masterOutputLevel.value = Math.max(-60, Math.min(0, rmsDb));
      if (peakDb > masterPeakLevel.value) {
        masterPeakLevel.value = Math.max(-60, Math.min(0, peakDb));
      } else {
        masterPeakLevel.value = Math.max(masterPeakLevel.value - 0.5, masterOutputLevel.value);
      }
    }
  });

  // When the project items finish streaming (or items are added/removed
  // by another client), re-resolve any pending activeCues that we
  // received cue_state for before the items existed locally.
  watch(() => currentProject.value?.items?.length, () => {
    // Sweep server.cues for anything Playing-like and ensure an entry.
    for (const sc of server.cues ?? []) {
      const t = (sc as any).transport;
      if (t == null || t === TRANSPORT_STOPPED) continue;
      // Already tracked by serverCueId?
      let tracked = false;
      for (const cue of activeCues.value.values()) {
        if (cue.serverCueId === sc.id) { tracked = true; break; }
      }
      if (tracked) continue;
      const item = findItemByServerCueId(sc.id);
      if (!item) continue;
      upsertActiveCue(item, t, (sc as any).playhead_seconds ?? 0, sc.id);
    }
    recomputeActiveGroups();
  });

  // ---- Transport intents (forward to server) -------------------------
  // Note: the server's WS `play` handler routes to trigger_item, which
  // dispatches audio→play_item and group→walk-startBehavior. So a single
  // playItem(uuid) is enough for both kinds of items.
  const playCue = async (item: AudioItem): Promise<boolean> => {
    if (!item || !item.uuid) return false;
    server.playItem(item.uuid);
    return true;
  };
  const triggerByUuid = (uuid: string) => {
    if (!uuid) return;
    server.playItem(uuid);
  };
  const triggerByIndex = (index: number[]) => {
    const item = findItemByIndex(index);
    if (item) server.playItem(item.uuid);
  };
  const triggerGroup = (group: GroupItem) => {
    if (group?.uuid) server.playItem(group.uuid);
  };

  const stopCue = async (uuid: string) => {
    if (!uuid) return;
    server.stopItem(uuid);
  };
  const stopAllCues = async () => { server.stopAll(0); };
  const panicStop   = async () => { server.stopAll(0); };

  const pauseCue = async (uuid: string) => {
    if (!uuid) return;
    server.pauseItem(uuid);
  };
  const resumeCue = async (uuid: string) => {
    if (!uuid) return;
    server.resumeItem(uuid);
  };

  // Seek is always in *absolute* file-time (matches the legacy contract).
  // Use the REST endpoint for a guaranteed ack; the WS path drops the
  // message silently if the cue isn't loaded yet.
  const seekCue = async (uuid: string, absoluteTime: number) => {
    if (!uuid) return;
    server.seekItem(uuid, Math.max(0, absoluteTime));
  };

  const setMasterGain = (db: number) => {
    const clamped = Math.max(-120, Math.min(12, db));
    masterGainDb.value = clamped;  // optimistic; server echoes back via doc_patch
    void server.setMasterGainDb(clamped);
  };

  const setNextItem = (uuid: string | null) => {
    nextItemOverrideUuid.value = uuid;
    server.setNextItem(uuid);
  };

  // ---- Custom action: http-request (client-side handler) ------------
  // The server's sequencer schedules custom actions and executes the
  // server-side ones directly (play-item / play-index / stop-all).
  // For http-request it broadcasts a doc_patch which lands here.
  async function executeHttpRequest(request: any) {
    if (!request || typeof request !== 'object' || !request.url) return;
    try {
      const options: RequestInit = {
        method: request.method || 'GET',
        headers: {},
      };
      if (request.body) {
        if (request.contentType === 'json') {
          (options.headers as any)['Content-Type'] = 'application/json';
          options.body = JSON.stringify(request.body);
        } else {
          (options.headers as any)['Content-Type'] =
            'application/x-www-form-urlencoded';
          options.body = new URLSearchParams(request.body).toString();
        }
      }
      await fetch(request.url, options);
    } catch (e) {
      console.warn('[useAudioEngine] custom http-request failed:', e);
    }
  }

  return {
    activeCues,
    activeGroups,
    masterOutputLevel,
    masterPeakLevel,
    masterGainDb,
    nextItemOverrideUuid,
    autoNextItemUuid,
    setMasterGain,
    setNextItem,
    playCue,
    stopCue,
    stopAllCues,
    panicStop,
    pauseCue,
    resumeCue,
    seekCue,
    triggerByUuid,
    triggerByIndex,
    triggerGroup,
  };
};

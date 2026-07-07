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
  playheadSeconds?: number; // absolute playhead in the file (independent of inPoint)
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

// Installed-once guard for the "cue first item on project open" watcher.
// useAudioEngine() is called from many components; without this the watcher
// would be registered once per consumer and arm the first item several times.
let firstCueWatcherInstalled = false;

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

  // Item uuids the operator explicitly stopped (stopCue / stop-all / panic).
  // The server's cue_state STOPPED edge is derived by diffing transport state
  // and carries no reason, so we record intent here: consulted + cleared when
  // the matching STOPPED edge arrives, letting maybeArmNextAfterStop tell a
  // manual stop from a natural end (#28 wrap-around rule).
  const manuallyStoppedUuids = useState<Set<string>>('manuallyStoppedUuids', () => new Set());

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
    view.playheadSeconds = playheadSeconds;
    view.isPaused    = (transport === TRANSPORT_PAUSED);
    // Map mutation: explicit set keeps reactivity in a Map<>.
    activeCues.value.set(item.uuid, view);
  };

  const removeActiveCue = (uuid: string) => {
    activeCues.value.delete(uuid);
  };

  // Issue #28 follow-up: the derived `autoNextItemUuid` only reflects the
  // CURRENTLY playing item, so once a 'nothing'-end cue finishes the "Up
  // Next" arming vanished and the operator's single-GO stepping broke after
  // the very first track. When the auto-cue setting is on, persist the
  // arming as a server-authoritative next-item override the moment the cue
  // stops, so "Up Next" survives past the end of the track. Guarded so we
  // never clobber a manual cue and never fire while other cues are still
  // playing (e.g. crossfades). Only 'nothing' items are handled here —
  // 'next'/'loop'/'goto' behaviours are auto-advanced by the server itself.
  //
  // `wasManual` distinguishes an operator stop from a natural end (see the
  // end-of-playlist wrap below): the two only differ once we fall off the end.
  const maybeArmNextAfterStop = (stoppedUuid: string | null, wasManual: boolean) => {
    if (!stoppedUuid) return;
    if (nextItemOverrideUuid.value) return;   // a manual / existing arming wins
    if (activeCues.value.size > 0) return;     // still playing something else
    if ((currentProject.value as any)?.settings?.autoCueNextWithoutEndBehavior === false) return;
    const item = findItemByUuid(stoppedUuid);
    if (!item || item.type !== 'audio') return;
    const audioItem = item as AudioItem;
    if (audioItem.endBehavior.action !== 'nothing') return;
    const nextIndex = [...audioItem.index];
    nextIndex[nextIndex.length - 1]++;
    const nextItem = findItemByIndex(nextIndex);
    if (nextItem) {
      // A following item exists → arm it. For a natural end this advances the
      // playlist; for a manual stop this is exactly the item that was already
      // showing as "Up Next" (autoNextItemUuid of the stopped cue), so the
      // arming is simply preserved across the stop.
      setNextItem(nextItem.uuid);
      return;
    }
    // No following item → we fell off the end of the playlist.
    //  - Natural end → wrap the arming back to the first item so a single GO
    //    restarts the show from the top. This ONLY arms it; playback still
    //    waits for the operator. A real auto-loop must be set intentionally
    //    via the last item's end behaviour (goto first) — never implied here.
    //  - Manual stop → leave "Up Next" as-is (there's nothing to keep), so
    //    stopping the last track to hold the show doesn't silently re-arm the
    //    top of the list under the operator.
    if (!wasManual) {
      const first = firstPlayableItem();
      if (first) setNextItem(first.uuid);
    }
  };

  // First audio item in document order (depth-first through groups). Used to
  // arm the very first GO target when a project opens.
  const firstPlayableItem = (): AudioItem | null => {
    const walk = (arr: (AudioItem | GroupItem)[]): AudioItem | null => {
      for (const it of arr) {
        if (it.type === 'audio') return it as AudioItem;
        if (it.type === 'group') {
          const hit = walk((it as GroupItem).children);
          if (hit) return hit;
        }
      }
      return null;
    };
    return currentProject.value ? walk(currentProject.value.items) : null;
  };

  // Issue #28 follow-up: cue the first playlist item when a project opens, so
  // the operator's very first GO fires without touching the mouse. Runs once
  // per project *open* (object-identity change), only after items have
  // streamed in, and only when nothing is already armed/playing — so it never
  // clobbers a manual cue, never fights a rejoined running session, and never
  // re-arms when items are added mid-show. Installed once (module guard) even
  // though useAudioEngine() has many callers.
  if (!firstCueWatcherInstalled) {
    firstCueWatcherInstalled = true;
    // Detached scope so the watcher lives for the app's lifetime rather than
    // being torn down when the first component that called useAudioEngine()
    // unmounts (which would leave the install-once flag stuck true).
    const scope = effectScope(true);
    scope.run(() => {
      let lastRef: unknown = null;
      let pendingFor: unknown = null;
      watch(
        () => [currentProject.value, currentProject.value?.items.length ?? 0] as const,
        ([proj, len]) => {
          if (proj !== lastRef) {
            // A different project object => a fresh open/create (or close→null).
            lastRef = proj;
            pendingFor = proj ?? null;
          }
          if (!proj || proj !== pendingFor) return;
          if (len === 0) return;          // wait for the first page of items
          pendingFor = null;              // only attempt once per open
          if ((proj as any)?.settings?.autoCueNextWithoutEndBehavior === false) return;
          if (nextItemOverrideUuid.value) return;   // already armed (manual / server)
          if (activeCues.value.size > 0) return;     // rejoined a running session
          const first = firstPlayableItem();
          if (first) setNextItem(first.uuid);
        },
        { immediate: true },
      );
    });
  }

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
        case 'loop':
          // A looping cue replays itself — surface it as its own "up next"
          // so the UI reflects the end behaviour instead of showing nothing.
          return audioItem.uuid;
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

      // Issue #28: an item WITHOUT an end behaviour still arms the next
      // playlist item as "Up Next" when the project setting is enabled
      // (default on, incl. legacy projects). This only arms the manual GO
      // target — the server never auto-advances a 'nothing' cue, so playback
      // still waits for the operator to press GO / spacebar. Lets an operator
      // step through a pre-ordered playlist with a single button.
      if (audioItem.endBehavior.action === 'nothing' &&
          (currentProject.value as any)?.settings?.autoCueNextWithoutEndBehavior !== false) {
        const nextIndex = [...audioItem.index];
        nextIndex[nextIndex.length - 1]++;
        const nextItem = findItemByIndex(nextIndex);
        if (nextItem) return nextItem.uuid;
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
      // Remove by item_uuid first (activeCues is keyed by item uuid and the
      // server now includes it) — mirroring the upsert path below. Matching
      // only on serverCueId is fragile: when a cue's serverCueId doesn't line
      // up with the event's cue_id (e.g. cart items, or a cue re-keyed across
      // a crossfade) the scan finds nothing and the entry lingers — which left
      // the outgoing item highlighted after a crossfade completed. Fall back to
      // the serverCueId scan for events that carry no item_uuid.
      let stoppedUuid: string | null = null;
      if (item_uuid && activeCues.value.has(item_uuid)) {
        stoppedUuid = item_uuid;
        removeActiveCue(item_uuid);
      } else {
        for (const [uuid, cue] of activeCues.value) {
          if (cue.serverCueId === cue_id) {
            stoppedUuid = uuid;
            removeActiveCue(uuid);
            break;
          }
        }
      }
      recomputeActiveGroups();
      // Set.delete returns true iff the uuid was queued by an operator stop —
      // that's our manual-vs-natural signal (and it clears the flag in one go).
      const wasManual = stoppedUuid ? manuallyStoppedUuids.value.delete(stoppedUuid) : false;
      maybeArmNextAfterStop(stoppedUuid, wasManual);
      return;
    }
    // Prefer item_uuid (server now includes it) so cart items without a
    // cueId annotation are still resolved. Fall back to cueId lookup.
    const item = (item_uuid ? findItemByUuid(item_uuid) : null) ?? findItemByServerCueId(cue_id);
    if (!item || item.type !== 'audio') return;
    upsertActiveCue(item as AudioItem, transport, playhead_seconds, cue_id);
    recomputeActiveGroups();
    // Consume the "Up Next" arming the moment the armed item actually starts —
    // via ANY path (GO button, the item's own play button, MIDI, cart, or the
    // server's own auto-advance), not only the GO button. Otherwise a manual
    // override still pointing at the now-playing item shadows the derived
    // autoNextItemUuid, and "Up Next" sticks on the item that's already
    // playing instead of advancing (the #28 open-a-project regression). Also
    // pushes the clear to the server so a later reconnect snapshot can't
    // restore the stale arming.
    if ((transport === TRANSPORT_PLAYING || transport === TRANSPORT_FADING_IN) &&
        nextItemOverrideUuid.value && nextItemOverrideUuid.value === item.uuid) {
      setNextItem(null);
    }
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
          cue.playheadSeconds = meter.playhead_seconds;
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
    // Flag the operator's intent so the incoming STOPPED edge is treated as a
    // manual stop (keep "Up Next" as-is) rather than a natural end.
    manuallyStoppedUuids.value.add(uuid);
    server.stopItem(uuid);
  };
  const stopAllCues = async () => {
    for (const uuid of activeCues.value.keys()) manuallyStoppedUuids.value.add(uuid);
    server.stopAll(0);
  };
  const panicStop   = async () => {
    for (const uuid of activeCues.value.keys()) manuallyStoppedUuids.value.add(uuid);
    server.stopAll(0);
  };

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

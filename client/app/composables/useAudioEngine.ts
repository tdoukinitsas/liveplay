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
import { runTime, remainingTime } from '~/utils/groupTiming';

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
  // Seconds left in the whole group (utils/groupTiming.ts).
  remaining: number;
  // The group contains a looping cue, so it has no real end.
  indefinite: boolean;
}

// Server's TransportState enum (mirrors C++).
const TRANSPORT_STOPPED   = 0;
const TRANSPORT_PLAYING   = 1;
const TRANSPORT_FADING_IN = 2;
const TRANSPORT_FADING_OUT= 3;
const TRANSPORT_PAUSED    = 4;

// Renderer-scoped guard for the WebSocket subscriptions below. Module scope,
// not per-call: the whole point is that repeated useAudioEngine() calls reuse
// one set of subscribers instead of stacking new ones. See the block it guards.
let _wsWired = false;

// cue-to-continue must never leave a permanent trace on the cue's saved
// End Behavior — it's a one-time "let this playing instance finish, then
// advance" action, not a reconfiguration. So instead of writing the resolved
// next/goto-item/goto-index target into endBehavior (which (a) is eligible
// for the server's Seamless Advance path — starts the next item ~0.1s
// *before* this pass's out-point for gapless "next"-chained cues, cutting the
// loop short, and (b) would permanently overwrite the cue's configured 'loop'
// with 'next', corrupting the show file for next time this cue is cued up),
// we arm 'nothing' (same value toggle-loop already uses, proven not to
// trigger any early-start path) to let the current pass finish for real, and
// remember BOTH the cue's original endBehavior and the resolved advance
// target here. removeActiveCue below restores the original endBehavior
// (leaving the saved cue exactly as the operator configured it) and plays
// the resolved target directly — never by writing it into endBehavior.
const pendingLoopContinuations = new Map<string, {
  originalEndBehavior: { action: string; targetUuid?: string; targetIndex?: number[] };
  advanceTarget: { action: 'next' | 'goto-item' | 'goto-index'; targetUuid?: string; targetIndex?: number[] };
}>();

// Shared by cue-to-continue and jump-cue (keyboard, MIDI, and the per-cue UI
// buttons): goto-item target → goto-index target → structural next, the same
// precedence endBehavior already supports for non-loop cues today.
export const resolveLoopContinuationTarget = (
  item: AudioItem
): { action: 'next' | 'goto-item' | 'goto-index'; targetUuid?: string; targetIndex?: number[] } => {
  const { targetUuid, targetIndex } = item.endBehavior;
  if (targetUuid) return { action: 'goto-item', targetUuid };
  if (targetIndex) return { action: 'goto-index', targetIndex };
  return { action: 'next' };
};

export const useAudioEngine = () => {
  const { currentProject, findItemByUuid, findItemByIndex, saveProject } = useProject();
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
  // server fans out as next_item_set doc_patch. All auto-cue arming decisions
  // (#28: advance-on-stop, first-item-on-open, end-of-list wrap) now live on
  // the authoritative server (ProjectState::arm_next_after_stop /
  // arm_first_item_on_open) so every connected client mirrors one decision
  // instead of each computing its own. The client only reflects this value.
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
    view.playheadSeconds = playheadSeconds;
    view.isPaused    = (transport === TRANSPORT_PAUSED);
    // Map mutation: explicit set keeps reactivity in a Map<>.
    activeCues.value.set(item.uuid, view);
  };

  // Fires the pending cue-to-continue advance: restores the cue's saved End
  // Behavior (never left mutated) and hard-starts the resolved next item.
  // Called the MOMENT the outgoing pass ends — see the two call sites below
  // for why that's "enters FadingOut" (or "reaches Stopped" when no fade is
  // configured at all), not "finishes fading out". Waiting for the fade to
  // fully complete before starting the next item is exactly the silent gap
  // this exists to avoid: the operator wants a hard start on the next cue
  // the instant this pass ends, with the outgoing cue's own configured fade
  // (if any) trailing underneath — not a fade-to-silence-then-start gap.
  const resolvePendingLoopContinuation = (uuid: string) => {
    const pending = pendingLoopContinuations.get(uuid);
    if (!pending) return;
    pendingLoopContinuations.delete(uuid);
    const { originalEndBehavior, advanceTarget } = pending;

    const item = findItemByUuid(uuid);
    // Restore exactly what the operator had configured — this action must
    // never leave a permanent trace on the cue's saved End Behavior. Pushed
    // immediately (not left to the generic 300ms-debounced item-diff watcher)
    // for the same reason queueLoopContinuation's arm does — see its comment.
    if (item && item.type === 'audio') {
      (item as AudioItem).endBehavior = { ...originalEndBehavior } as AudioItem['endBehavior'];
      saveProject();
      server.updateProjectItem(uuid, { endBehavior: (item as AudioItem).endBehavior }).catch(() => {});
    }

    let nextItem: AudioItem | GroupItem | null = null;
    if (advanceTarget.action === 'goto-item' && advanceTarget.targetUuid) {
      nextItem = findItemByUuid(advanceTarget.targetUuid);
    } else if (advanceTarget.action === 'goto-index' && advanceTarget.targetIndex) {
      nextItem = findItemByIndex(advanceTarget.targetIndex);
    } else if (item) {
      const nextIndex = [...item.index];
      nextIndex[nextIndex.length - 1]++;
      nextItem = findItemByIndex(nextIndex);
    }
    if (nextItem) {
      if (nextItem.type === 'audio') playCue(nextItem as AudioItem);
      else if (nextItem.type === 'group') triggerGroup(nextItem);
    }
  };

  const removeActiveCue = (uuid: string) => {
    activeCues.value.delete(uuid);
    // Covers cues with no fadeOutDuration configured at all: they go
    // Playing → Stopped directly, skipping FadingOut entirely, so this is
    // the only edge that ever fires for them.
    resolvePendingLoopContinuation(uuid);
  };

  // Used by cue-to-continue (keyboard + MIDI): let the current loop pass
  // finish for real, then advance directly to `advanceTarget` — never by
  // writing it into endBehavior. See the pendingLoopContinuations comment
  // above for why.
  const queueLoopContinuation = (
    item: AudioItem,
    advanceTarget: { action: 'next' | 'goto-item' | 'goto-index'; targetUuid?: string; targetIndex?: number[] }
  ) => {
    if (!activeCues.value.has(item.uuid)) {
      // Not currently playing — nothing to finish. Advance directly, same as
      // removeActiveCue's post-stop step, without ever touching endBehavior.
      let nextItem: AudioItem | GroupItem | null = null;
      if (advanceTarget.action === 'goto-item' && advanceTarget.targetUuid) {
        nextItem = findItemByUuid(advanceTarget.targetUuid);
      } else if (advanceTarget.action === 'goto-index' && advanceTarget.targetIndex) {
        nextItem = findItemByIndex(advanceTarget.targetIndex);
      } else {
        const nextIndex = [...item.index];
        nextIndex[nextIndex.length - 1]++;
        nextItem = findItemByIndex(nextIndex);
      }
      if (nextItem) {
        if (nextItem.type === 'audio') playCue(nextItem as AudioItem);
        else if (nextItem.type === 'group') triggerGroup(nextItem);
      }
      return;
    }
    pendingLoopContinuations.set(item.uuid, {
      originalEndBehavior: { ...item.endBehavior },
      advanceTarget,
    });
    item.endBehavior = { action: 'nothing' };
    saveProject();
    // Push this to the server RIGHT NOW rather than waiting on the generic
    // item-diff watcher (debounced 300ms): that watcher is fine for ordinary
    // editing, but this write disables server-side looping (set_loop(false))
    // and a short loop (a few seconds) can wrap back around before a
    // 300ms-delayed push ever lands — the pass loops one more time and the
    // operator sees the button flip back to "loop" as if the press did
    // nothing. A direct, immediate PATCH closes that race.
    server.updateProjectItem(item.uuid, { endBehavior: item.endBehavior }).catch(() => {});
  };

  // jump-cue (keyboard, MIDI, per-cue UI button): stop `item` right now and
  // start whatever it would have advanced to — never touches endBehavior.
  const jumpCue = (item: AudioItem) => {
    const advanceTarget = resolveLoopContinuationTarget(item);
    let nextItem: AudioItem | GroupItem | null = null;
    if (advanceTarget.action === 'goto-item' && advanceTarget.targetUuid) {
      nextItem = findItemByUuid(advanceTarget.targetUuid);
    } else if (advanceTarget.action === 'goto-index' && advanceTarget.targetIndex) {
      nextItem = findItemByIndex(advanceTarget.targetIndex);
    } else {
      const nextIndex = [...item.index];
      nextIndex[nextIndex.length - 1]++;
      nextItem = findItemByIndex(nextIndex);
    }
    stopCue(item.uuid);
    if (nextItem) {
      if (nextItem.type === 'audio') playCue(nextItem as AudioItem);
      else if (nextItem.type === 'group') triggerGroup(nextItem);
    }
  };

  // Note: the auto-cue "Up Next" arming that used to live here (advance on
  // stop, first-item on open, end-of-list wrap) is now owned entirely by the
  // server so multiple clients share one authoritative decision. The client
  // only reflects `nextItemOverrideUuid` (pushed via next_item_set / snapshot)
  // and the deterministic `autoNextItemUuid` display below.

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
    // Every group with anything playing anywhere inside it, nested groups
    // included, timed as a whole (utils/groupTiming.ts): play-all groups last
    // as long as their longest child, play-first groups add up.
    const walk = (group: GroupItem) => {
      for (const child of group.children) {
        if (child.type === 'group') walk(child as GroupItem);
      }
      const remaining = remainingTime(group, activeCues.value);
      if (remaining === null) return;
      const total = runTime(group);
      next.set(group.uuid, {
        uuid: group.uuid,
        displayName: group.displayName,
        totalDuration: Math.max(total.seconds, remaining),
        currentTime: Math.max(0, total.seconds - remaining),
        remaining,
        indefinite: total.indefinite,
      });
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
      // Only a cue that is really in the playlist has a "next" in the running
      // order. findItemByUuid also returns cart-only sounds, whose index is
      // [-1, slot] and names no playlist position, and a stale index would be
      // just as wrong — so the item must actually sit where its index says.
      if (!Array.isArray(audioItem.index) || findItemByIndex(audioItem.index)?.uuid !== uuid) continue;
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
  // Installed exactly once per renderer. Everything below writes only to
  // useState-backed shared state, so a single set of subscribers serves every
  // caller — and a single set is the only safe number. useAudioEngine() is
  // called per playlist row, per cart slot, per active cue, and from inside
  // event handlers (MainWorkspace's F1 path), and none of these ever
  // unsubscribed. The meters broadcast arrives at frame rate, so each leaked
  // subscriber became permanent per-frame work: re-mounting the playlist —
  // exactly what session recovery does — multiplied the fan-out by the row
  // count and left the UI lagging for the rest of the session.
  if (import.meta.client && !_wsWired) {
    _wsWired = true;
    // Detached effect scope. The first caller is nearly always a component,
    // and a bare watch() here would be owned by that component and stopped the
    // moment it unmounts — silently taking the now-shared subscription with it.
    effectScope(true).run(() => {

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
      if (item_uuid && activeCues.value.has(item_uuid)) {
        removeActiveCue(item_uuid);
      } else {
        for (const [uuid, cue] of activeCues.value) {
          if (cue.serverCueId === cue_id) {
            removeActiveCue(uuid);
            break;
          }
        }
      }
      recomputeActiveGroups();
      // "Up Next" arming after a stop is decided by the server (it distinguishes
      // manual stops from natural ends and owns the end-of-list wrap), then
      // mirrored back to us via a next_item_set doc_patch.
      return;
    }
    // Prefer item_uuid (server now includes it) so cart items without a
    // cueId annotation are still resolved. Fall back to cueId lookup.
    const item = (item_uuid ? findItemByUuid(item_uuid) : null) ?? findItemByServerCueId(cue_id);
    if (!item || item.type !== 'audio') return;
    // FadingOut is the earliest signal that this pass has genuinely ended —
    // fire the pending cue-to-continue advance here so the next cue hard-
    // starts immediately, with the outgoing cue's own fade (if any, even
    // just the 1s default every new item gets) trailing underneath rather
    // than gating when the next cue starts.
    if (transport === TRANSPORT_FADING_OUT) resolvePendingLoopContinuation(item.uuid);
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

    });
  }   // end one-time WS wiring

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
    // The server treats a single-cue stop as a manual stop for Up-Next arming.
    server.stopItem(uuid);
  };

  // A cart slot may hold a whole group (discussion #61), so the slot, its
  // hotkey and its MIDI note fire / stop / test "an item", whichever it is.
  // A group is playing while anything inside it is; stopping it stops every
  // cue inside that is playing, which also ends its run (a manual stop fires
  // no end behaviour).
  const isItemPlaying = (item: AudioItem | GroupItem | null): boolean => {
    if (!item) return false;
    return item.type === 'group'
      ? activeGroups.value.has(item.uuid)
      : activeCues.value.has(item.uuid);
  };
  const fireItem = (item: AudioItem | GroupItem) => {
    if (item.type === 'group') triggerGroup(item as GroupItem);
    else playCue(item as AudioItem);
  };
  const stopItemAny = (item: AudioItem | GroupItem) => {
    if (item.type !== 'group') { stopCue(item.uuid); return; }
    const walk = (g: GroupItem) => {
      for (const child of g.children) {
        if (child.type === 'group') walk(child as GroupItem);
        else if (activeCues.value.has(child.uuid)) stopCue(child.uuid);
      }
    };
    walk(item as GroupItem);
  };
  // Global Stop All — omit the fade so the server applies the project-wide
  // Stop All fade (settings.stopAllFadeMs, default 1 s). Set that to 0 in
  // Project Settings for an instant panic.
  const stopAllCues = async () => {
    server.stopAll();
  };
  const panicStop   = async () => {
    server.stopAll();
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
    queueLoopContinuation,
    jumpCue,
    findParentGroup,
    isItemPlaying,
    fireItem,
    stopItemAny,
  };
};

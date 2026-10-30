// =====================================================================
// useEvents.ts
// ---------------------------------------------------------------------
// Client side of the event system (#84). The server owns the timing and
// fires every event; this keeps what the editors need to show:
//
//   * plugins     — each plugin's schema, from GET /api/events/plugins.
//                   Refetched on connect and whenever an editor opens,
//                   because the MIDI port lists inside it are live.
//   * connections — the show's destinations. Read from the project document
//                   (useProject mirrors them there, since a whole-document
//                   save would otherwise wipe them), written through PUT.
//   * lastResult / flashing — fed by the `event_fired` doc_patch, so a
//                   marker can blink as it fires and a failed one can say why.
//
// Nothing about an item's events is kept here: they live on the item and
// travel with it through the ordinary item diff, like any other field.
//
// Wired once per renderer from plugins/liveplay-server.client.ts, so a
// failure during the show is on record before anyone opens the Events tab.
// =====================================================================
import type { AudioItem, Project } from '~/types/project';
import type {
  EventConnection,
  EventFiredPatch,
  EventTestRequest,
  EventTestResult,
  PluginSchema,
  FieldSchema,
  EventOptionsContext,
  EventOptionsResult,
  DiscoveredDevice,
} from '~/types/events';
import { localizeSchema } from '~/utils/events';

export interface EventResult {
  ok: boolean;
  error?: string;
  /** Date.now() when the server reported it. */
  at: number;
}

// Long enough to be seen from across a desk, short enough that a burst of
// events a beat apart still reads as separate blinks.
const FLASH_MS = 450;

let _wired = false;
let _pluginsInFlight: Promise<void> | null = null;
// Live option lists (see fetchOptions), shared across every form.
const OPTIONS_TTL_MS = 10_000;
const _optionsCache = new Map<string, { at: number; result: EventOptionsResult }>();
const _optionsInFlight = new Map<string, Promise<EventOptionsResult>>();

export const useEvents = () => {
  const server = useLiveplayServer();
  // The key useProject owns. Read directly rather than through useProject(),
  // because this is also wired from the plugin before the app has mounted.
  const currentProject = useState<Project | null>('currentProject', () => null);
  // useCartItems' store, by key for the same reason.
  const cartStore = useState<Map<string, AudioItem>>('useCartItems.cartOnlyItems', () => new Map());

  const plugins = useState<PluginSchema[]>('events.plugins', () => []);
  // 'unknown' until the first answer; 'unsupported' is an older server with
  // no events route, which the editors explain instead of showing empty forms.
  const pluginsStatus = useState<'unknown' | 'ok' | 'unsupported' | 'error'>(
    'events.pluginsStatus', () => 'unknown');
  const lastResult = useState<Record<string, EventResult>>('events.lastResult', () => ({}));
  const flashing = useState<Set<string>>('events.flashing', () => new Set());

  // What every editor reads: the server's schemas with the built-in plugins'
  // wording in the operator's language (see localizeSchema).
  const { t } = useLocalization();
  const tr = (key: string) => { const v = t(key); return v === key ? undefined : v; };
  const localizedPlugins = computed<PluginSchema[]>(() => plugins.value.map(p => localizeSchema(p, tr)));

  const connections = computed<EventConnection[]>(
    () => currentProject.value?.eventConnections ?? []);

  async function refreshPlugins(): Promise<void> {
    // Opening the tab and the pane together would otherwise ask twice.
    if (_pluginsInFlight) return _pluginsInFlight;
    _pluginsInFlight = (async () => {
      try {
        plugins.value = await server.fetchEventPlugins();
        pluginsStatus.value = 'ok';
      } catch (e: any) {
        if (/^404\b/.test(String(e?.message ?? ''))) {
          plugins.value = [];
          pluginsStatus.value = 'unsupported';
        } else {
          // Keep the last good list: a dropped request mid-edit shouldn't
          // empty the form someone is filling in.
          if (pluginsStatus.value === 'unknown') pluginsStatus.value = 'error';
          console.warn('[events] plugin refresh failed:', e);
        }
      } finally {
        _pluginsInFlight = null;
      }
    })();
    return _pluginsInFlight;
  }

  /**
   * Replace the show's connection list. Adopted locally from the reply as
   * well as from the broadcast, so the next save carries it even if the
   * broadcast is still on its way. Throws on failure; callers show it.
   */
  async function saveConnections(list: EventConnection[]): Promise<EventConnection[]> {
    const saved = await server.saveEventConnections(JSON.parse(JSON.stringify(list)));
    if (currentProject.value) currentProject.value.eventConnections = saved;
    // The server's document has it already; this is the disk write, under
    // the same autosave rules as any other edit.
    void useProject().saveProject();
    return saved;
  }

  /** Fire once, now, without a cue playing. Never throws. */
  async function testEvent(
    event: EventTestRequest['event'],
    opts: { connection?: EventConnection; itemUuid?: string } = {},
  ): Promise<EventTestResult> {
    try {
      const res = await server.testEvent({
        event: JSON.parse(JSON.stringify(event)),
        ...(opts.connection ? { connection: JSON.parse(JSON.stringify(opts.connection)) } : {}),
        ...(opts.itemUuid ? { itemUuid: opts.itemUuid } : {}),
      });
      return { ok: !!res?.ok, error: res?.error };
    } catch (e: any) {
      return { ok: false, error: e?.message ?? String(e) };
    }
  }

  /**
   * A field's live options (`optionsFrom`), read from the device through the
   * server. Cached for a few seconds and shared between callers, because one
   * screen can hold several forms asking the same mixer the same question.
   * `fresh` skips the cache (the field's refresh button).
   */
  async function fetchOptions(
    ctx: EventOptionsContext,
    field: FieldSchema,
    values: Record<string, unknown>,
    fresh = false,
  ): Promise<EventOptionsResult> {
    if (!field.optionsFrom) return { options: field.options ?? [] };
    const deps: Record<string, unknown> = {};
    for (const d of field.dependsOn ?? []) deps[d] = values[d];
    const connection = ctx.connection ? JSON.parse(JSON.stringify(ctx.connection)) : undefined;
    const key = JSON.stringify([ctx.plugin, field.optionsFrom, ctx.connectionId ?? null,
                                connection?.config ?? null, deps]);
    const hit = _optionsCache.get(key);
    if (!fresh && hit && Date.now() - hit.at < OPTIONS_TTL_MS) return hit.result;
    const pending = _optionsInFlight.get(key);
    if (pending) return pending;
    const p = (async () => {
      try {
        const result = await server.fetchEventOptions({
          plugin: ctx.plugin, source: field.optionsFrom!,
          ...(connection ? { connection } : { connectionId: ctx.connectionId ?? null }),
          values: deps,
        });
        const clean: EventOptionsResult = {
          options: Array.isArray(result?.options) ? result.options : (field.options ?? []),
          error: result?.error,
        };
        _optionsCache.set(key, { at: Date.now(), result: clean });
        return clean;
      } catch (e: any) {
        return { options: field.options ?? [], error: e?.message ?? String(e) };
      } finally {
        _optionsInFlight.delete(key);
      }
    })();
    _optionsInFlight.set(key, p);
    return p;
  }

  /** Devices the plugin finds on the server's networks. */
  async function discover(plugin: string): Promise<DiscoveredDevice[]> {
    return await server.discoverEventDevices(plugin);
  }

  /**
   * How many events use each connection, across every item including the
   * cart's own cues. One walk for the whole list, so a pane showing a count
   * per connection doesn't walk the project once per row.
   */
  function connectionUsage(): Map<string, number> {
    const counts = new Map<string, number>();
    const p = currentProject.value;
    if (!p) return counts;
    const seen = new Set<string>();
    const walk = (arr: any[] | undefined) => {
      for (const it of arr ?? []) {
        if (!it || seen.has(it.uuid)) continue;
        seen.add(it.uuid);
        if (Array.isArray(it.events)) {
          for (const ev of it.events) {
            if (ev?.connectionId) counts.set(ev.connectionId, (counts.get(ev.connectionId) ?? 0) + 1);
          }
        }
        if (it.type === 'group') walk(it.children);
      }
    };
    walk(p.items);
    // The cart store can be ahead of the document's cartOnlyItems until the
    // next save mirrors it, so walk it first; `seen` keeps the two from
    // counting the same cue twice.
    walk(Array.from(cartStore.value.values()));
    walk(p.cartOnlyItems);
    return counts;
  }

  const isFlashing = (eventId: string) => flashing.value.has(eventId);

  if (!_wired && import.meta.client) {
    _wired = true;
    const flashTimers = new Map<string, ReturnType<typeof setTimeout>>();

    const flash = (eventId: string) => {
      const prev = flashTimers.get(eventId);
      if (prev) clearTimeout(prev);
      // A new Set each time rather than mutating: every reader re-renders
      // without relying on collection reactivity through useState.
      flashing.value = new Set(flashing.value).add(eventId);
      flashTimers.set(eventId, setTimeout(() => {
        flashTimers.delete(eventId);
        const next = new Set(flashing.value);
        next.delete(eventId);
        flashing.value = next;
      }, FLASH_MS));
    };

    server.onDocPatch((p: any) => {
      if (!p || p.op !== 'event_fired' || typeof p.eventId !== 'string') return;
      const fired = p as EventFiredPatch;
      lastResult.value = {
        ...lastResult.value,
        [fired.eventId]: { ok: !!fired.ok, error: fired.error, at: Date.now() },
      };
      flash(fired.eventId);
    });

    // A results map from the last show means nothing in the next one.
    watch(() => currentProject.value?.folderPath, () => { lastResult.value = {}; });

    watch(() => server.connected, (c) => { if (c) void refreshPlugins(); }, { immediate: true });
  }

  return {
    plugins: localizedPlugins,
    pluginsStatus,
    connections,
    lastResult,
    flashing,
    isFlashing,
    refreshPlugins,
    saveConnections,
    testEvent,
    fetchOptions,
    discover,
    connectionUsage,
  };
};

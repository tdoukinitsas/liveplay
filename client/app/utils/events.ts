// =====================================================================
// utils/events.ts
// ---------------------------------------------------------------------
// Pure helpers for the event system (#84): schema defaults and visibility,
// list ordering, the one-line summary, and the mm:ss.mmm time format. No
// reactivity and no server calls, so the forms, the Events tab and the
// connections pane all read the schemas the same way.
// =====================================================================
import { v4 as uuidv4 } from 'uuid';
import type {
  ActionSchema,
  EventConnection,
  EventTrigger,
  FieldSchema,
  ItemEvent,
  PluginSchema,
} from '~/types/events';

const clone = <T,>(v: T): T => (v === undefined ? v : JSON.parse(JSON.stringify(v)));

/** What a field holds before anyone has touched it. */
export function fieldDefault(field: FieldSchema): unknown {
  if (field.default !== undefined) return clone(field.default);
  switch (field.type) {
    case 'boolean': return false;
    case 'list': return [];
    default: return undefined;
  }
}

/** A fresh value object with every field's default filled in. */
export function defaultsFor(fields: FieldSchema[] | undefined): Record<string, unknown> {
  return fillDefaults(fields, {});
}

/**
 * `values` with a default for every field it lacks. Existing values are kept
 * as they are, including ones for fields the schema no longer lists: a newer
 * server's option should survive a round trip through an older form.
 */
export function fillDefaults(
  fields: FieldSchema[] | undefined,
  values: Record<string, unknown> | undefined,
): Record<string, unknown> {
  const out: Record<string, unknown> = { ...(values ?? {}) };
  for (const f of fields ?? []) {
    if (out[f.id] === undefined) {
      const d = fieldDefault(f);
      if (d !== undefined) out[f.id] = d;
    }
  }
  return out;
}

/** showIf: compared against the sibling's value, or its default while unset. */
export function isFieldVisible(
  field: FieldSchema,
  siblings: FieldSchema[],
  values: Record<string, unknown>,
): boolean {
  if (!field.showIf) return true;
  let v = values[field.showIf.field];
  if (v === undefined) {
    const sib = siblings.find(s => s.id === field.showIf!.field);
    if (sib) v = fieldDefault(sib);
  }
  // Loose on type: a select hands back "1" where the schema may say 1.
  return field.showIf.equals.some(e => e === v || String(e) === String(v));
}

export const findPlugin = (plugins: PluginSchema[], id: string | undefined) =>
  plugins.find(p => p.id === id) ?? null;

export const findAction = (plugin: PluginSchema | null, id: string | undefined): ActionSchema | null =>
  plugin?.actions.find(a => a.id === id) ?? null;

/** Connections a plugin's events may use. */
export const connectionsFor = (connections: EventConnection[], pluginId: string) =>
  connections.filter(c => c.plugin === pluginId);

/**
 * Fill an action's summary template ("{address} {args}") from the options.
 * Enum values show their label, lists their row count, unknown ids nothing.
 */
export function formatSummary(action: ActionSchema | null, options: Record<string, unknown>): string {
  if (!action?.summary) return '';
  const byId = new Map(action.fields.map(f => [f.id, f]));
  return action.summary.replace(/\{([\w.-]+)\}/g, (_m, id: string) => {
    const v = options?.[id];
    if (v === undefined || v === null) return '';
    const f = byId.get(id);
    if (f?.type === 'enum') {
      const opt = f.options?.find(o => String(o.value) === String(v));
      if (opt) return opt.label;
    }
    if (Array.isArray(v)) return String(v.length);
    if (typeof v === 'object') return '';
    return String(v);
  }).replace(/\s+/g, ' ').trim();
}

// Roughly the order a cue lives through them, so the list reads like the
// cue's own timeline; time events sort among themselves by position.
const TRIGGER_RANK: Record<EventTrigger, number> = { start: 0, time: 1, pause: 2, resume: 3, end: 4 };

/** List order: start, then time events by time, then pause, resume, end. */
export function sortEvents(events: ItemEvent[]): ItemEvent[] {
  return [...events].sort((a, b) => {
    const r = TRIGGER_RANK[a.trigger] - TRIGGER_RANK[b.trigger];
    if (r !== 0) return r;
    if (a.trigger === 'time') return (a.time ?? 0) - (b.time ?? 0);
    return 0;
  });
}

/** 83.25 → "1:23.250". Minutes are not padded or capped. */
export function formatEventTime(seconds: number | undefined | null): string {
  const s = Math.max(0, Number(seconds) || 0);
  const totalMs = Math.round(s * 1000);
  const mins = Math.floor(totalMs / 60000);
  const secs = Math.floor((totalMs % 60000) / 1000);
  const ms = totalMs % 1000;
  return `${mins}:${String(secs).padStart(2, '0')}.${String(ms).padStart(3, '0')}`;
}

/**
 * Parse what someone typed as a time: "1:23.250", "83.25", "1:02:03.5".
 * Returns null for anything that isn't one, so the caller can put the old
 * value back rather than jumping the marker to zero.
 */
export function parseEventTime(raw: string): number | null {
  const text = raw.trim().replace(',', '.');
  if (!text) return null;
  const parts = text.split(':');
  if (parts.length > 3 || parts.some(p => p === '' || !/^\d*\.?\d*$/.test(p))) return null;
  let total = 0;
  for (const p of parts) total = total * 60 + Number(p);
  return Number.isFinite(total) ? total : null;
}

/** A new event with the chosen plugin / action's defaults. */
export function makeEvent(
  trigger: EventTrigger,
  plugin: PluginSchema,
  connections: EventConnection[],
  time?: number,
): ItemEvent {
  const action = eventActions(plugin)[0];
  const conn = plugin.needsConnection ? connectionsFor(connections, plugin.id)[0] : undefined;
  const ev: ItemEvent = {
    id: uuidv4(),
    enabled: true,
    trigger,
    plugin: plugin.id,
    action: action?.id ?? '',
    connectionId: conn?.id ?? null,
    options: defaultsFor(action?.fields),
  };
  if (trigger === 'time') ev.time = time ?? 0;
  return ev;
}

/** The actions an event may use: all but the test-only ones. */
export function eventActions(plugin: PluginSchema | null | undefined): ActionSchema[] {
  return (plugin?.actions ?? []).filter(a => !a.testOnly);
}

/** Deep copies with fresh ids, for an item cloned from another. */
export function copyEventsWithNewIds(events: ItemEvent[] | undefined): ItemEvent[] | undefined {
  if (!Array.isArray(events)) return events;
  return events.map(ev => ({ ...clone(ev), id: uuidv4() }));
}

// ---- Localising plugin schemas ----------------------------------------------
// Schemas come from the server in English, and a plugin needs no client code.
// The built-in plugins' wording is translated by overlaying locale keys:
//
//   eventPlugins.<plugin>.label | .description
//   eventPlugins.<plugin>.actions.<action>          action label
//   eventPlugins.<plugin>.summaries.<action>        list summary template
//   eventPlugins.<plugin>.connection.<field>.label | .help
//   eventPlugins.<plugin>.fields.<field>.label | .help   (list rows too)
//   eventPlugins.<plugin>.options.<field>.<value>   enum option label
//
// Anything without a key keeps the server's English, so a plugin nobody has
// translated yet still reads fine. Values ('.' replaced by '_') and ids are
// never translated: they are what gets saved.
export function localizeSchema(
  plugin: PluginSchema,
  tr: (key: string) => string | undefined,
): PluginSchema {
  const base = `eventPlugins.${plugin.id}`;
  const field = (f: FieldSchema, ns: 'fields' | 'connection'): FieldSchema => {
    const k = `${base}.${ns}.${f.id}`;
    return {
      ...f,
      label: tr(`${k}.label`) ?? f.label,
      help: f.help === undefined ? undefined : (tr(`${k}.help`) ?? f.help),
      options: f.options?.map(o => ({
        ...o,
        label: tr(`${base}.options.${f.id}.${String(o.value).replace(/\./g, '_')}`) ?? o.label,
      })),
      fields: f.fields?.map(sub => field(sub, ns)),
    };
  };
  return {
    ...plugin,
    label: tr(`${base}.label`) ?? plugin.label,
    description: plugin.description === undefined ? undefined
      : (tr(`${base}.description`) ?? plugin.description),
    connectionFields: (plugin.connectionFields ?? []).map(f => field(f, 'connection')),
    actions: (plugin.actions ?? []).map(a => ({
      ...a,
      label: tr(`${base}.actions.${a.id}`) ?? a.label,
      summary: a.summary === undefined ? undefined : (tr(`${base}.summaries.${a.id}`) ?? a.summary),
      fields: (a.fields ?? []).map(f => field(f, 'fields')),
    })),
  };
}

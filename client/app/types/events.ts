// =====================================================================
// types/events.ts
// ---------------------------------------------------------------------
// The event system (#84): things an audio cue does to the outside world —
// fire an OSC message at the lighting desk at 1:32.250, send a MIDI note when
// it starts, call a URL when it stops.
//
// The SERVER owns all of it: it keeps the timing, fires the events and
// publishes what each plugin can do as a schema. The client only edits the
// data and renders the forms from those schemas, so a new plugin on the
// server needs no client release.
// =====================================================================

/** What makes an event fire. Only `time` has a position on the waveform. */
export type EventTrigger = 'time' | 'start' | 'end' | 'pause' | 'resume';

export const EVENT_TRIGGERS: EventTrigger[] = ['time', 'start', 'pause', 'resume', 'end'];

/**
 * A destination for a plugin's events, defined once per show ("Lighting desk:
 * OSC 192.168.1.50:8000") and picked by any number of events. Lives on the
 * project root as `eventConnections`, beside `items` and `settings`.
 */
export interface EventConnection {
  id: string;
  name: string;
  plugin: string;
  /** Absent = true. A disabled connection silently drops its events. */
  enabled?: boolean;
  config: Record<string, unknown>;
}

/** One event on an audio item (`AudioItem.events`). */
export interface ItemEvent {
  id: string;
  name?: string;
  /** Absent = true. */
  enabled?: boolean;
  trigger: EventTrigger;
  /**
   * Seconds on the FILE timeline, the same convention as inPoint / outPoint /
   * startNextTime. Only meaningful for `time`; a time outside the trimmed
   * region never fires, but is kept so re-widening the trim brings it back.
   */
  time?: number;
  plugin: string;
  action: string;
  connectionId?: string | null;
  options: Record<string, unknown>;
}

export type FieldType =
  | 'string' | 'text' | 'number' | 'integer' | 'boolean' | 'enum' | 'item' | 'list';

export interface FieldOption {
  value: string | number;
  label: string;
}

/**
 * One form field, as the server describes it. `item` is a picker over the
 * project's audio items and groups whose value is the uuid; `list` is a
 * repeatable group of `fields` whose value is an array of row objects.
 */
export interface FieldSchema {
  id: string;
  label: string;
  type: FieldType;
  default?: unknown;
  required?: boolean;
  min?: number;
  max?: number;
  step?: number;
  placeholder?: string;
  help?: string;
  options?: FieldOption[];
  /** enum: the user may also type a value not in the list (a MIDI port that isn't plugged in). */
  allowCustom?: boolean;
  fields?: FieldSchema[];
  /** Show only while the sibling field's value is one of `equals`. */
  showIf?: { field: string; equals: unknown[] };
  /**
   * enum: read the options live from the device (POST /api/events/options),
   * with `options` as the fallback while that's unavailable.
   */
  optionsFrom?: string;
  /** optionsFrom: sibling fields whose values change the list (refetch on change). */
  dependsOn?: string[];
  /** Display unit shown after the input ("dB", "s"). */
  unit?: string;
  /**
   * number, in seconds: how long the action takes once fired (a fade). The
   * Events timeline draws it as a band from the event's marker.
   */
  timelineSpan?: boolean;
}

export interface ActionSchema {
  id: string;
  label: string;
  fields: FieldSchema[];
  /** One-line template for the list, e.g. "{address}", filled from the options. */
  summary?: string;
  /**
   * Only for the connection editor's Test, which picks it first; never offered
   * for events. For a harmless check, so testing a connection can't change a
   * device (a mixer's first action may be "recall snapshot").
   */
  testOnly?: boolean;
}

export interface PluginSchema {
  id: string;
  label: string;
  /** Material Symbols name. */
  icon: string;
  description?: string;
  /** false for plugins with no outside destination (the internal `liveplay` one). */
  needsConnection: boolean;
  connectionFields: FieldSchema[];
  actions: ActionSchema[];
  /** The plugin can find devices on the network (POST /api/events/discover). */
  discoverable?: boolean;
}

/** Where a live option list comes from: a saved connection, or a draft one. */
export interface EventOptionsContext {
  plugin: string;
  connectionId?: string | null;
  connection?: EventConnection | null;
}

/** Reply of POST /api/events/options. */
export interface EventOptionsResult {
  options: FieldOption[];
  error?: string;
}

/** One device from POST /api/events/discover. */
export interface DiscoveredDevice {
  label: string;
  name: string;
  config: Record<string, unknown>;
}

/** Body of POST /api/events/test. */
export interface EventTestRequest {
  event: Pick<ItemEvent, 'plugin' | 'action' | 'options'> & { connectionId?: string | null };
  /** An unsaved draft, so a connection can be tested while it is being edited. */
  connection?: EventConnection;
  itemUuid?: string;
}

export interface EventTestResult {
  ok: boolean;
  error?: string;
}

/** doc_patch `event_fired`: one event has just been fired by the server. */
export interface EventFiredPatch {
  op: 'event_fired';
  itemUuid: string;
  eventId: string;
  trigger: EventTrigger;
  ok: boolean;
  error?: string;
}

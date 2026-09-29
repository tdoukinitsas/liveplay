// =====================================================================
// multiEdit.ts
// ---------------------------------------------------------------------
// Editing several playlist items at once from the Properties panel.
//
// The panel always draws ONE item — the primary selection — and its edit
// handlers keep writing to that one object, exactly as they do for a single
// selection. What makes it a batch edit is what happens on save: every field
// that changed on the primary (and every field the operator committed while it
// showed "Multiple values") is copied onto every other selected item it means
// something on. Field by field, never whole objects: changing a cue's end
// action does not also overwrite the target each of the others jumps to.
//
// The same field test decides what the panel shows. A field whose value differs
// across the selection reads "Multiple values" until somebody sets it.
// =====================================================================
import type { AudioItem, BaseItem } from '~/types/project';
import { DEFAULT_AUDIO_ITEM, DEFAULT_GROUP_ITEM } from '~/types/project';

/** Identity, structure and media: what makes an item THAT item. Never copied. */
const NEVER_SHARED = new Set([
  'uuid', 'index', 'type', 'children', 'isExpanded',
  'mediaPath', 'mediaServerPath', 'mediaFileName', 'duration',
  'waveform', 'channelPeaks',
]);

/**
 * Fields that mean the same thing on a group and on a cue. Everything else
 * (timing, fades, ducking, LTC, start behaviour — whose actions differ by type)
 * is copied only between items of the same type as the one on screen.
 */
const CROSS_TYPE = new Set(['displayName', 'color', 'busId', 'endBehavior']);

/** Times measured into the media: each item's own length bounds them. */
const TIME_KEYS = new Set(['inPoint', 'outPoint', 'startNextTime']);

const isPlainObject = (v: unknown): v is Record<string, unknown> =>
  !!v && typeof v === 'object' && !Array.isArray(v);

export function getPath(obj: any, path: string): unknown {
  return path.split('.').reduce((o, k) => (o == null ? undefined : o[k]), obj);
}

function hasPath(obj: any, path: string): boolean {
  const keys = path.split('.');
  const last = keys.pop()!;
  const parent = keys.reduce((o, k) => (o == null ? undefined : o[k]), obj);
  return parent != null && Object.prototype.hasOwnProperty.call(parent, last);
}

function setPath(obj: any, path: string, value: unknown) {
  const keys = path.split('.');
  const last = keys.pop()!;
  let o = obj;
  for (const k of keys) {
    if (!isPlainObject(o[k])) o[k] = {};
    o = o[k];
  }
  o[last] = value;
}

function deletePath(obj: any, path: string) {
  const keys = path.split('.');
  const last = keys.pop()!;
  const parent = keys.reduce((o, k) => (o == null ? undefined : o[k]), obj);
  if (parent != null) delete parent[last];
}

/** What an item shows for a field it has never had set. */
function defaultFor(item: BaseItem, path: string): unknown {
  const base = item.type === 'group' ? DEFAULT_GROUP_ITEM : DEFAULT_AUDIO_ITEM;
  const d = getPath(base, path);
  if (d !== undefined) return d;
  // Optional fields the panel renders with a fixed default.
  switch (path) {
    case 'busId': return '';
    case 'advanceDelay': return 0;
    case 'ltcEnabled': return false;
    case 'ltcStartTimecode': return '00:00:00:00';
    case 'ltcFrameRate': return 4;
    case 'outPoint': return (item as AudioItem).duration;
    default: return null;
  }
}

/** Every leaf path whose value differs between two versions of one item. */
export function changedPaths(before: any, after: any, prefix = ''): string[] {
  const out: string[] = [];
  const keys = new Set([...Object.keys(before ?? {}), ...Object.keys(after ?? {})]);
  for (const k of keys) {
    const path = prefix ? `${prefix}.${k}` : k;
    if (!prefix && NEVER_SHARED.has(k)) continue;
    const a = before?.[k], b = after?.[k];
    if (isPlainObject(a) && isPlainObject(b)) out.push(...changedPaths(a, b, path));
    else if (JSON.stringify(a) !== JSON.stringify(b)) out.push(path);
  }
  return out;
}

/** Does this field mean anything on `item`, given the type of the one on screen? */
export function fieldApplies(item: BaseItem, path: string, primaryType: string, value?: unknown): boolean {
  const top = path.split('.')[0]!;
  if (NEVER_SHARED.has(top)) return false;
  if (item.type === primaryType) return true;
  if (!CROSS_TYPE.has(top)) return false;
  // A group cannot loop; every other end action is common to both.
  if (path === 'endBehavior.action' && value === 'loop' && item.type === 'group') return false;
  return true;
}

/** True when the selected items do not all agree on this field. */
export function isFieldMixed(items: BaseItem[], primary: BaseItem | null, path: string): boolean {
  if (!primary || items.length < 2) return false;
  let first: string | undefined;
  for (const it of items) {
    if (!fieldApplies(it, path, primary.type)) continue;
    const v = getPath(it, path);
    const s = JSON.stringify(v === undefined ? defaultFor(it, path) : v);
    if (first === undefined) first = s;
    else if (s !== first) return true;
  }
  return false;
}

/**
 * Copy `paths` from `primary` onto every other item in `items` they apply to.
 * A path the primary no longer has is removed from the others too (clearing a
 * bus assignment back to "inherit", for instance).
 */
export function propagateFields(primary: BaseItem, items: BaseItem[], paths: Iterable<string>) {
  for (const path of paths) {
    const present = hasPath(primary, path);
    const value = getPath(primary, path);
    for (const item of items) {
      if (item === primary || item.uuid === primary.uuid) continue;
      if (!fieldApplies(item, path, primary.type, value)) continue;
      if (!present) { deletePath(item, path); continue; }

      let v: unknown = value === undefined ? value : JSON.parse(JSON.stringify(value));
      if (TIME_KEYS.has(path) && item.type === 'audio' && typeof v === 'number') {
        const a = item as AudioItem;
        v = Math.max(0, Math.min(v, a.duration || v));
        // An in point past this item's out point (or the reverse) would leave
        // it nothing to play; that item keeps its own value instead.
        if (path === 'inPoint' && (v as number) >= (a.outPoint || a.duration)) continue;
        if (path === 'outPoint' && (v as number) <= (a.inPoint || 0)) continue;
      }
      setPath(item, path, v);
    }
  }
}


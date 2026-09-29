// Run time of items and groups, and how much of a playing group is left
// (#63, discussion #61).
//
// A group's run time is an ESTIMATE of how long it plays when fired, built
// from what the document says rather than from anything the server knows:
//   - an audio item runs for its trimmed length (in -> out), plus its "wait
//     before next" when another cue in the group follows it;
//   - a play-first group plays its children one after another, so their run
//     times add up;
//   - a play-all group starts every child at once and lasts as long as the
//     longest.
// It assumes the children follow on in order, which is what a playlist group
// is for; a goto that jumps around inside the group makes it a guess. A cue
// that loops has no end, so any total containing one is `indefinite` — the
// figure is then how long the finite part lasts, a lower bound.
import type { AudioItem, GroupItem } from '~/types/project';

type Item = AudioItem | GroupItem;

export interface RunTime {
  seconds: number;
  indefinite: boolean;
}

// What the timing needs to know about a playing cue: its trimmed length and
// how far into it the playhead is, both relative to the in-point.
export interface PlayingCue {
  duration: number;
  currentTime: number;
}

export const trimmedLength = (a: AudioItem): number =>
  Math.max(0, (a.outPoint || a.duration || 0) - (a.inPoint || 0));

const isSequential = (g: GroupItem) => g.startBehavior?.action !== 'play-all';

// The wait an item adds before whatever follows it, when something does.
const gapAfter = (item: Item): number =>
  item.type === 'audio' ? Math.max(0, (item as AudioItem).advanceDelay || 0) : 0;

export function runTime(item: Item): RunTime {
  if (item.type === 'audio') {
    const a = item as AudioItem;
    return { seconds: trimmedLength(a), indefinite: a.endBehavior?.action === 'loop' };
  }
  const g = item as GroupItem;
  const parts = (g.children || []).map(runTime);
  const indefinite = parts.some(p => p.indefinite);
  if (!isSequential(g)) {
    return { seconds: parts.reduce((m, p) => Math.max(m, p.seconds), 0), indefinite };
  }
  let seconds = 0;
  g.children.forEach((child, i) => {
    seconds += parts[i]!.seconds;
    if (i < g.children.length - 1) seconds += gapAfter(child);
  });
  return { seconds, indefinite };
}

// Seconds left in `item`, or null when nothing inside it is playing.
export function remainingTime(item: Item, playing: Map<string, PlayingCue>): number | null {
  if (item.type === 'audio') {
    const c = playing.get(item.uuid);
    return c ? Math.max(0, c.duration - c.currentTime) : null;
  }
  const g = item as GroupItem;
  const children = g.children || [];
  if (!isSequential(g)) {
    let best: number | null = null;
    for (const child of children) {
      const r = remainingTime(child, playing);
      if (r !== null) best = best === null ? r : Math.max(best, r);
    }
    return best;
  }
  // The LAST child with something playing is where the group has got to — an
  // earlier one may still be crossfading out.
  let at = -1;
  let left = 0;
  children.forEach((child, i) => {
    const r = remainingTime(child, playing);
    if (r !== null) { at = i; left = r; }
  });
  if (at < 0) return null;
  for (let i = at; i < children.length; i++) {
    if (i > at) left += runTime(children[i]!).seconds;
    if (i < children.length - 1) left += gapAfter(children[i]!);
  }
  return left;
}

// "1:05", "1:02:03". Floors, as the cue rows do.
export function formatRunTime(seconds: number): string {
  const total = Math.max(0, Math.floor(seconds));
  const h = Math.floor(total / 3600);
  const m = Math.floor((total % 3600) / 60);
  const s = total % 60;
  return h > 0
    ? `${h}:${m.toString().padStart(2, '0')}:${s.toString().padStart(2, '0')}`
    : `${m}:${s.toString().padStart(2, '0')}`;
}

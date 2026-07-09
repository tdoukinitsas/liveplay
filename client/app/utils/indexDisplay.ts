export function normalizeIndexDisplayStart(value: unknown): number {
  const n = Number(value);
  if (!Number.isFinite(n)) return 0;
  return Math.max(0, Math.trunc(n));
}

function isSpecialIndexPath(index: number[] | undefined | null): boolean {
  return Array.isArray(index) && index.length > 0 && index[0] < 0;
}

export function toDisplayIndexPath(index: number[], start: unknown): number[] {
  if (isSpecialIndexPath(index)) return [...index];

  const offset = normalizeIndexDisplayStart(start);
  return index.map(part => part + offset);
}

export function fromDisplayIndexPath(displayIndex: number[], start: unknown): number[] {
  const offset = normalizeIndexDisplayStart(start);
  return displayIndex
    .map(part => part - offset)
    .filter(part => Number.isInteger(part) && part >= 0);
}

export function formatDisplayIndexPath(index: number[] | undefined | null, start: unknown): string {
  if (!index || index.length === 0) return '';
  return toDisplayIndexPath(index, start).join(',');
}

export function parseDisplayIndexPath(raw: string, start: unknown): number[] {
  const displayIndex = raw
    .split(/[.,/]/)
    .map(s => s.trim())
    .filter(Boolean)
    .map(s => Number.parseInt(s, 10))
    .filter(n => Number.isFinite(n));

  return fromDisplayIndexPath(displayIndex, start);
}
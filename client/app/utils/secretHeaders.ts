// =============================================================================
// utils/secretHeaders.ts — which HTTP header values are credentials (F-23)
// -----------------------------------------------------------------------------
// The client mirror of server/include/liveplay/core/secret_headers.hpp. The
// server never sends a credential to any client: it sends SECRET_MASK in its
// place, and a SECRET_MASK sent back means "keep the stored value". The client
// only needs the rule to know which values to type as passwords.
// =============================================================================

/** What the server sends in place of a secret value. */
export const SECRET_MASK = '••••••';

const EXACT = new Set(['authorization', 'proxy-authorization', 'cookie', 'set-cookie']);
const PARTS = ['token', 'key', 'secret', 'pass', 'auth', 'session', 'signature', 'credential', 'apikey'];

/** True for a header whose value is a credential (Authorization, X-API-Key, …). */
export const isSecretHeader = (name: string): boolean => {
  const n = name.trim().toLowerCase();
  if (!n) return false;
  return EXACT.has(n) || PARTS.some(p => n.includes(p));
};

export interface HeaderRow {
  name: string;
  value: string;
  /** A line with no colon, kept exactly as it came. */
  raw?: string;
}

export const parseHeaders = (text: string): HeaderRow[] =>
  (text ?? '').split('\n').map(line => line.replace(/\r$/, '')).filter(line => line.trim() !== '')
    .map(line => {
      const colon = line.indexOf(':');
      if (colon === -1) return { name: '', value: '', raw: line };
      return { name: line.slice(0, colon).trim(), value: line.slice(colon + 1).trim() };
    });

export const formatHeaders = (rows: HeaderRow[]): string =>
  rows.filter(r => r.raw !== undefined || r.name.trim() || r.value.trim())
    .map(r => (r.raw !== undefined ? r.raw : `${r.name.trim()}: ${r.value.trim()}`))
    .join('\n');

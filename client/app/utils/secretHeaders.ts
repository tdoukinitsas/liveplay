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

/**
 * A base URL as the server compares it for "still the same destination"
 * (secret_headers.hpp normalise_base_url): "host:80", "http://host" and
 * "HTTP://Host/" are one place. A stored credential is only ever sent to the
 * place it was given for, so moving a connection asks for it again (R2-01).
 */
export const normaliseBaseUrl = (input: unknown): string => {
  let url = typeof input === 'string' ? input.trim() : '';
  url = url.replace(/\/+$/, '');
  if (!url) return url;
  if (!url.includes('://')) url = `http://${url}`;
  const schemeEnd = url.indexOf('://');
  const hostStart = schemeEnd + 3;
  let hostEnd = url.indexOf('/', hostStart);
  if (hostEnd === -1) hostEnd = url.length;
  const scheme = url.slice(0, schemeEnd).toLowerCase();
  let host = url.slice(hostStart, hostEnd).toLowerCase();
  const defaultPort = scheme === 'https' ? ':443' : scheme === 'http' ? ':80' : '';
  if (defaultPort && host.endsWith(defaultPort)) host = host.slice(0, -defaultPort.length);
  return `${scheme}://${host}${url.slice(hostEnd)}`;
};

export const formatHeaders = (rows: HeaderRow[]): string =>
  rows.filter(r => r.raw !== undefined || r.name.trim() || r.value.trim())
    .map(r => (r.raw !== undefined ? r.raw : `${r.name.trim()}: ${r.value.trim()}`))
    .join('\n');

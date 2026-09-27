// Presentation only. Contract text and examples live in reference.json.
const byId = id => document.getElementById(id);
const slug = value => value.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '');
const node = (tag, text, parent) => {
  const el = document.createElement(tag);
  if (text !== undefined) el.textContent = text;
  if (parent) parent.append(el);
  return el;
};
const paragraph = (parent, text, className) => {
  if (!text) return;
  const p = node('p', text, parent);
  if (className) p.className = className;
};
const code = (parent, value) => node('code', typeof value === 'string' ? value : JSON.stringify(value, null, 2), node('pre', undefined, parent));
function table(parent, headings, rows) {
  if (!rows.length) return;
  const wrap = node('div', undefined, parent);
  wrap.className = 'table-wrap';
  const t = node('table', undefined, wrap);
  const head = node('tr', undefined, node('thead', undefined, t));
  headings.forEach(h => { const th = node('th', h, head); th.scope = 'col'; });
  const body = node('tbody', undefined, t);
  rows.forEach(row => {
    const tr = node('tr', undefined, body);
    row.forEach(cell => node('td', String(cell), tr));
  });
}
const fieldTable = (parent, rows) => table(parent, ['Field', 'Type', 'Required', 'Default / omitted', 'Description'], rows);
function schemaLink(parent, name) {
  if (!name) return;
  const p = node('p', 'Complete schema: ', parent);
  const a = node('a', name, p);
  a.href = `#schema-${slug(name.replace(/\[\]$/, ''))}`;
}
function card(parent, id, label) {
  const details = node('details', undefined, parent);
  details.id = id;
  details.className = 'entry';
  node('summary', label, details);
  const body = node('div', undefined, details);
  body.className = 'detail-body';
  const permalink = node('a', 'Link to this entry', body);
  permalink.href = `#${id}`;
  return body;
}
function relatedLinks(parent, entry) {
  for (const name of entry.relatedSchemas || []) schemaLink(parent, name);
  for (const [label, href] of entry.links || []) node('a', label, node('p', undefined, parent)).href = href;
}
function entrySections(parent, entry, schemas) {
  for (const section of entry.sections || []) {
    const details = node('details', undefined, parent);
    node('summary', section.title, details);
    const body = node('div', undefined, details);
    body.className = 'detail-body';
    section.notes.forEach(note => paragraph(body, note));
    if (section.schema) {
      const schema = schemas.find(s => s.name === section.schema);
      fieldTable(body, schema.fields.filter(field => section.fieldPrefixes.some(prefix => field[0].startsWith(prefix))));
    }
    for (const example of section.examples) {
      node('h4', example.title, body);
      code(body, requestExample({ ...entry, request: example.body }));
    }
  }
}
function responseFields(value, prefix = '') {
  if (value === null) return [[prefix || '(body)', 'null', 'See notes for non-null variants']];
  if (Array.isArray(value)) {
    return [[prefix || '(body)', 'array', value.length ? 'Element fields below' : 'Element shape in notes/shared schema'], ...(value.length ? responseFields(value[0], `${prefix}[]`) : [])];
  }
  if (typeof value === 'object') return Object.entries(value).flatMap(([key, v]) => responseFields(v, prefix ? `${prefix}.${key}` : key));
  return [[prefix || '(body)', typeof value === 'number' ? 'number' : typeof value, 'See example and behavioral notes']];
}
function requestExample(e) {
  let path = e.path.replace('<string>', 'RESOURCE_ID').replace('<int>', '0').replace('<path>', e.path.startsWith('/static') ? 'example.txt' : '0/1');
  const queries = [];
  for (const field of e.fields) {
    if (!field[0].startsWith('?')) continue;
    const key = field[0].slice(1);
    const value = key === 'path' ? (e.path === '/api/fs/list' ? 'C:/Shows/Show' : 'C:/Shows/Show/media/opening.wav') : key === 'token' ? 'DOWNLOAD_TOKEN' : key === 'buckets' ? '2' : key === 'reset' ? '1' : field[3];
    queries.push(`${key}=${encodeURIComponent(value)}`);
  }
  if (queries.length) path += '?' + queries.join('&');
  let result = `curl -X ${e.methods[0]} 'http://127.0.0.1:4480${path}'`;
  if (e.auth !== 'Public') result += " \\\n  -H 'Authorization: Bearer TOKEN'";
  if (e.path === '/api/upload') result += " \\\n  -F 'file=@opening.wav'";
  else if (e.request !== null) result += " \\\n  -H 'Content-Type: application/json' \\\n  --data '" + JSON.stringify(e.request).replace(/'/g, "'\\''") + "'";
  return result;
}
function openHash() {
  const target = document.getElementById(decodeURIComponent(location.hash.slice(1)));
  if (!target) return;
  for (let parent = target; parent; parent = parent.parentElement) {
    if (parent.tagName === 'DETAILS') parent.open = true;
    parent.hidden = false;
  }
  target.scrollIntoView({ block: 'start' });
}
try {
  const response = await fetch(new URL('./reference.json', import.meta.url));
  if (!response.ok) throw new Error(`HTTP ${response.status}`);
  const data = await response.json();
  const categories = [...new Set(data.endpoints.map(e => e.category))];
  for (const category of categories) {
    const id = `rest-${slug(category)}`;
    const section = node('section', undefined, byId('endpoint-list'));
    section.className = 'category';
    section.id = id;
    node('h3', category, section);
    const a = node('a', category, byId('categories'));
    a.href = `#${id}`;
    for (const e of data.endpoints.filter(e => e.category === category)) {
      const body = card(section, `http-${slug(e.methods[0] + '-' + e.path)}`, `${e.methods.join(' / ')} ${e.path} — ${e.title}`);
      paragraph(body, `Authentication: ${e.auth} when auth is enabled. API token: ${e.apiToken}.`, 'meta');
      if (e.fields.length) fieldTable(body, e.fields);
      else paragraph(body, 'No request body or query parameters. See notes for path parameters.');
      paragraph(body, e.notes);
      relatedLinks(body, e);
      entrySections(body, e, data.schemas);
      node('h4', 'Example request', body);
      code(body, requestExample(e));
      node('h4', '200 response', body);
      code(body, e.response);
      schemaLink(body, e.responseSchema);
      if (!e.responseSchema && typeof e.response !== 'string') table(body, ['Response field', 'Type', 'Notes'], responseFields(e.response));
      node('h4', 'Errors', body);
      paragraph(body, 'Common authentication/framework/uncaught-error behavior is defined in HTTP conventions. Additional validation/store errors may be described in the notes above.');
      table(body, ['Status', 'Handler error / condition'], e.errors);
      if (e.sourceLine) {
        const source = node('a', 'Handler source', body);
        source.href = `https://github.com/tdoukinitsas/liveplay/blob/main/server/src/net/control_server.cpp#L${e.sourceLine}`;
      } else paragraph(body, e.source, 'meta');
    }
  }
  for (const [key, listId, prefix] of [['commands', 'command-list', 'command'], ['events', 'event-list', 'event'], ['patches', 'patch-list', 'patch']]) {
    for (const e of data.websocket[key]) {
      const name = e.type || e.op;
      const body = card(byId(listId), `${prefix}-${slug(name)}`, `${name} — ${e.purpose}`);
      paragraph(body, key === 'commands' ? 'Direction: Client → Server. type is a required string.' : 'Direction: Server → Client. type is a required string; doc_patch also requires op.', 'meta');
      fieldTable(body, e.fields);
      paragraph(body, e.notes);
      if (e.reply) paragraph(body, `Expected response: ${e.reply}`);
      schemaLink(body, e.schema);
      relatedLinks(body, e);
      code(body, e.example);
    }
  }
  for (const s of data.schemas) {
    const body = card(byId('schema-list'), `schema-${slug(s.name)}`, s.name);
    paragraph(body, s.description);
    relatedLinks(body, s);
    fieldTable(body, s.fields);
    if (s.example) {
      node('h4', 'Example', body);
      code(body, s.example);
    }
  }
  const entries = [...document.querySelectorAll('.entry')];
  const searchText = new Map(entries.map(e => [e, e.textContent.toLowerCase()]));
  const updateFilter = () => {
    const query = byId('filter').value.toLowerCase().trim();
    let count = 0;
    for (const entry of entries) {
      entry.hidden = !searchText.get(entry).includes(query);
      if (!entry.hidden) count++;
    }
    document.querySelectorAll('.category').forEach(s => { s.hidden = ![...s.querySelectorAll('.entry')].some(e => !e.hidden); });
    byId('status').textContent = `${count} of ${entries.length} reference entries shown.`;
  };
  byId('filter').addEventListener('input', updateFilter);
  const setOpen = (entry, open) => {
    entry.open = open;
    entry.querySelectorAll('details').forEach(detail => { detail.open = open; });
  };
  byId('expand').addEventListener('click', () => entries.filter(e => !e.hidden).forEach(e => setOpen(e, true)));
  byId('collapse').addEventListener('click', () => entries.forEach(e => setOpen(e, false)));
  const printableDetails = [...document.querySelectorAll('.entry, .entry details')];
  let printState;
  window.addEventListener('beforeprint', () => { printState = printableDetails.map(e => e.open); entries.filter(e => !e.hidden).forEach(e => setOpen(e, true)); });
  window.addEventListener('afterprint', () => { printableDetails.forEach((e, i) => { e.open = printState[i]; }); });
  window.addEventListener('hashchange', openHash);
  const methodCount = data.endpoints.reduce((sum, e) => sum + e.methods.length, 0);
  byId('counts').textContent = `${methodCount} HTTP method/path combinations (${methodCount - 1} application, 1 framework static route); ${data.websocket.commands.length} client command types; ${data.websocket.events.length} server frame types, including ${data.websocket.patches.length} doc_patch operations. All are documented here.`;
  updateFilter();
  if (location.hash) openHash();
} catch (error) {
  byId('status').textContent = `Could not load the reference: ${error.message}. Use the reference data link below or reload the page.`;
}

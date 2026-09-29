// Read-only documentation consistency check; no application behavior is exercised.
import assert from 'node:assert/strict';
import { readFile, readdir } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const read = path => readFile(resolve(root, path), 'utf8');
const data = JSON.parse(await read('docs-site/public/api/reference.json'));
async function sources(dir) {
  const result = [];
  for (const entry of await readdir(resolve(root, dir), { withFileTypes: true })) {
    const path = `${dir}/${entry.name}`;
    if (entry.isDirectory()) result.push(...await sources(path));
    else if (/\.(cpp|hpp|h|cc|cxx)$/.test(path)) result.push([path, await read(path)]);
  }
  return result;
}
// Preserve quoted strings while removing comments, including commented examples.
const stripComments = text => text.replace(/"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|\/\/[^\n]*|\/\*[\s\S]*?\*\//g,
  match => match.startsWith('//') || match.startsWith('/*') ? match.replace(/[^\n]/g, ' ') : match);
const sourceFiles = [...await sources('server/src'), ...await sources('server/include')]
  .map(([path, content]) => [path, stripComments(content)]);
const registered = [];
const upgrades = [];
let registrations = 0;
for (const [path, content] of sourceFiles) {
  for (const match of content.matchAll(/CROW_ROUTE\s*\(\s*\w+\s*,\s*"([^"]+)"\s*\)\s*(?:\.methods\(([^)]*)\))?/g)) {
    registrations++;
    const methods = match[2] ? [...match[2].matchAll(/HTTPMethod::(\w+)/g)].map(m => m[1].toUpperCase()) : ['GET'];
    assert(methods.length, `Unrecognized method declaration: ${path}: ${match[0]}`);
    registered.push(...methods.map(method => `${method} ${match[1]}`));
  }
  for (const match of content.matchAll(/CROW_WEBSOCKET_ROUTE\s*\(\s*\w+\s*,\s*"([^"]+)"/g)) upgrades.push(match[1]);
  assert(!/\b(?:route_dynamic|route_static|register_blueprint)\s*\(/.test(content), `Review new registration mechanism in ${path}`);
}
const unique = (values, label) => {
  assert.equal(new Set(values).size, values.length, `Duplicate ${label}`);
  return new Set(values);
};
const same = (actual, expected, label) => assert.deepEqual([...actual].sort(), [...expected].sort(), label);
unique(registered, 'source route');
const catalogRoutes = data.endpoints.flatMap(e => e.methods.map(m => `${m} ${e.path}`));
unique(catalogRoutes, 'documented route');
same(catalogRoutes, [...registered, 'GET /static/<path>'], 'HTTP source/catalog coverage');
same(upgrades, ['/ws'], 'WebSocket upgrades require overview review');

const control = sourceFiles.find(([p]) => p === 'server/src/net/control_server.cpp')[1];
const dispatchStart = control.indexOf('static std::string handle_ws_message');
assert(dispatchStart >= 0, 'Find the WebSocket dispatcher');
const dispatch = control.slice(dispatchStart, control.indexOf('CROW_ROUTE', dispatchStart));
const commands = new Set([...dispatch.matchAll(/\btype\s*==\s*"([^"]+)"/g)].map(m => m[1]));
const events = new Set();
const patches = new Set();
for (const [path, content] of sourceFiles) {
  // Discovery emits a UDP beacon, not a WebSocket frame.
  if (path === 'server/src/net/discovery.cpp') continue;
  for (const match of content.matchAll(/\{\s*"type"\s*,\s*"([^"]+)"/g)) events.add(match[1]);
  // These are the current frame-building variables; schema entry.type is HTTP metadata.
  for (const match of content.matchAll(/\b(?:payload|evt|patch)\s*\["type"\]\s*=\s*"([^"]+)"/g)) events.add(match[1]);
  for (const match of content.matchAll(/\{\s*"op"\s*,\s*"([^"]+)"|\["op"\]\s*=\s*"([^"]+)"/g)) patches.add(match[1] || match[2]);
}
same(commands, unique(data.websocket.commands.map(e => e.type), 'command'), 'Client command coverage');
same(events, unique(data.websocket.events.map(e => e.type), 'event'), 'Server frame coverage');
same(patches, unique(data.websocket.patches.map(e => e.op), 'patch operation'), 'Document notification coverage');

const schemas = unique(data.schemas.map(s => s.name), 'schema');
const allEntries = [...data.endpoints, ...data.schemas, ...Object.values(data.websocket).flat()];
for (const entry of allEntries) {
  for (const field of entry.fields) assert.equal(field.length, 5, `Field table: ${entry.path || entry.name || entry.type || entry.op}`);
  for (const name of [entry.schema, entry.responseSchema].filter(Boolean)) assert(schemas.has(name.replace(/\[\]$/, '')), `Missing schema ${name}`);
}
for (const e of data.endpoints) {
  for (const key of ['methods', 'path', 'title', 'category', 'auth', 'apiToken', 'fields', 'request', 'response', 'notes', 'errors']) assert(key in e, `${e.path}: missing ${key}`);
  for (const row of e.errors) assert.equal(row.length, 2, `${e.path}: error table`);
}
for (const [kind, entries] of Object.entries(data.websocket)) {
  for (const e of entries) {
    assert(e.example.type, `${kind}: example envelope`);
    assert.equal(e.example.type, kind === 'patches' ? 'doc_patch' : e.type);
    if (kind === 'patches') assert.equal(e.example.op, e.op);
  }
}
let sectionCount = 0;
for (const entry of allEntries) {
  for (const name of entry.relatedSchemas || []) assert(schemas.has(name), `Missing related schema ${name}`);
  for (const section of entry.sections || []) {
    sectionCount++;
    if (section.schema) {
      const schema = data.schemas.find(s => s.name === section.schema);
      assert(schema, `Missing section schema ${section.schema}`);
      for (const prefix of section.fieldPrefixes) assert(schema.fields.some(f => f[0].startsWith(prefix)), `Unknown field prefix ${prefix}`);
    }
    for (const example of section.examples) {
      assert('body' in example, 'HTTP example body required');
      assert(entry.path && entry.methods, 'Request examples must belong to an HTTP endpoint');
    }
  }
}
console.log(`HTTP: ${registrations} application registrations, ${registered.length} method/path pairs; plus 1 Crow static route = ${catalogRoutes.length} documented.`);
console.log(`WebSocket: ${commands.size} commands, ${events.size} server frame types, ${patches.size} doc_patch operations; all documented.`);
console.log(`Catalog: ${data.schemas.length} shared schemas; field tables, envelopes and schema references valid.`);
console.log(`Endpoint detail sections: ${sectionCount}; schema references and request examples valid.`);
console.log('Crow implicit behavior is reviewed separately; this check does not parse arbitrary C++ or verify live behavior.');

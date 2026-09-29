// No audio required. Run against a disposable server: this replaces its project.
// Usage: node server/tests/e2e/preview-sends-e2e.js [port]
const assert = require('node:assert/strict');
const base = `http://127.0.0.1:${process.argv[2] || 4580}`;
async function request(path, method = 'GET', body) {
  const response = await fetch(base + path, {
    method, headers: { 'content-type': 'application/json' },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  return { status: response.status, body: await response.json() };
}
const patch = (id, body) => request(`/api/buses/${id}`, 'PATCH', body);
const sends = id => [{ id, levelDb: -6, tap: 'post' }];

(async () => {
  assert.equal((await request('/api/project/document', 'PUT', {
    name: 'preview-sends-regression', items: [],
  })).status, 200);
  const create = async name => {
    const r = await request('/api/buses', 'POST', {
      name, output: { type: 'output', target: 'Unmapped test output' },
    });
    assert.equal(r.status, 200);
    return r.body.id;
  };
  const candidate = await create('Candidate');
  const other = await create('Other');
  const oldPreview = (await request('/api/buses')).body.find(b => b.preview).id;

  // Incoming sends, including those from the master, must block promotion.
  const master = (await request('/api/buses')).body.find(b => b.master).id;
  for (const source of [other, master]) {
    assert.equal((await patch(source, { sends: sends(candidate) })).status, 200);
    const before = (await request('/api/buses')).body;
    assert.equal((await patch(candidate, { preview: true })).status, 409);
    assert.deepEqual((await request('/api/buses')).body, before);
    assert.equal((await patch(source, { sends: [] })).status, 200);
  }

  assert.equal((await patch(candidate, { sends: sends(other) })).status, 200);
  let before = (await request('/api/buses')).body;
  assert.equal((await patch(candidate, { preview: true })).status, 409);
  assert.deepEqual((await request('/api/buses')).body, before);

  // Validate the resulting state: an atomic clear-and-promote is legal.
  assert.equal((await patch(candidate, { preview: true, sends: [] })).status, 200);
  assert.equal((await request(`/api/buses/${candidate}`)).body.preview, true);
  assert.equal((await patch(oldPreview, { preview: true })).status, 200);

  // Adding sends in the promotion request must not evade the same rule.
  before = (await request('/api/buses')).body;
  assert.equal((await patch(candidate, { preview: true, sends: sends(other) })).status, 409);
  assert.deepEqual((await request('/api/buses')).body, before);
  console.log('PASS: Preview promotion checks incoming, outgoing and simultaneous sends; refused patches are atomic.');
})().catch(error => { console.error(error); process.exitCode = 1; });

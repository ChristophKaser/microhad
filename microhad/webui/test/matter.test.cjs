const { test } = require('node:test');
const assert = require('node:assert/strict');
const http = require('node:http');
const wire = require(process.env.MATTER_WEBUI_MODULE);
const id = '1234567890abcdef';
const session = { session: id, state: 'waiting_ble', deadline_remaining_seconds: 120 };
const thread = { enabled: true, available: true, configured: true, attached: false, state: 'attaching', linux_ipv6_or_tun: false };
const accepted = { schema: 1, ok: true, job: 1, state: 'queued' };
const completed = result => ({ job: 1, state: 'complete', response: { schema: 1, ok: true, result } });

async function peer(handler, action) {
  const calls = [];
  const server = http.createServer(async (request, response) => {
    let body = '';
    for await (const chunk of request) body += chunk;
    calls.push({ method: request.method, path: request.url, body: body ? JSON.parse(body) : undefined });
    const reply = handler(calls.at(-1), calls);
    response.writeHead(reply.status || 200, { 'Content-Type': 'application/json' });
    response.end(typeof reply.body === 'string' ? reply.body : JSON.stringify(reply.body));
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const original = global.fetch;
  global.fetch = (url, options) => original(`http://127.0.0.1:${server.address().port}${url}`, options);
  try { await action(calls); }
  finally { global.fetch = original; server.closeAllConnections(); await new Promise(resolve => server.close(resolve)); }
}
const signal = () => new AbortController().signal;

test('bounded session and dataset inputs reject noncanonical values', () => {
  assert.equal(wire.validSession(id), true);
  for (const value of ['0000000000000000', id.toUpperCase(), id + '0', 'x']) assert.equal(wire.validSession(value), false);
  assert.equal(wire.validDataset('00'.repeat(254)), true);
  for (const value of ['', 'A0', '0', '00'.repeat(255)]) assert.equal(wire.validDataset(value), false);
  for (const value of [{ ...session, state: 'invented' }, { ...session, deadline_remaining_seconds: 901 },
    { ...session, node: 'secret' }, { ...session, pin: 20202021 }]) assert.throws(() => wire.sessionResult(value), /protocol/);
});

test('disabled status and enabled unconfigured Thread remain distinct', async () => {
  let enabled = false;
  await peer(call => ({ body: enabled ? call.path === '/api/thread/status' ? { ...thread, configured: false } : { enabled: true, available: true, controller_initialized: true, fabric_ready: true, state: 'running' } : { enabled: false, available: false } }), async () => {
    assert.equal((await wire.matterAvailability(signal())).matter.enabled, false);
    enabled = true;
    const next = await wire.matterAvailability(signal());
    assert.equal(next.matter.enabled, true); assert.equal(next.thread.configured, false);
  });
});

test('queue delivery completion is never accessory commissioning completion', async () => {
  let polls = 0;
  await peer(call => ({ body: call.method === 'POST' ? accepted : ++polls === 1 ? { job: 1, state: 'running' } : completed(session) }), async calls => {
    const value = wire.sessionResult(await wire.matterCall('/api/matter/commissioning/status', { session: id }, signal()));
    assert.equal(wire.terminalSession(value), false);
    assert.deepEqual(calls[0].body, { schema: 1, session: id });
    assert.equal(calls.filter(call => call.method === 'POST').length, 1);
    assert.equal(polls, 2);
  });
});

test('Thread import uses bounded transient JSON without channel overrides', async () => {
  await peer(call => ({ body: call.method === 'POST' ? accepted : completed(thread) }), async calls => {
    await wire.matterCall('/api/thread/import', { dataset: '01020b00' }, signal());
    assert.deepEqual(calls[0].body, { schema: 1, dataset: '01020b00' });
    await assert.rejects(wire.matterCall('/api/thread/import', { dataset: 'abc' }, signal()), /malformed/);
    await assert.rejects(wire.matterCall('/api/thread/import', { dataset: '01020b00', zigbee_channel: 11 }, signal()), /malformed/);
    assert.equal(calls.filter(call => call.method === 'POST').length, 1);
  });
});

test('known-session cancellation returns terminal result and is sent once', async () => {
  await peer(call => ({ body: call.method === 'POST' ? accepted : completed({ ...session, state: 'cancelled', deadline_remaining_seconds: 0 }) }), async calls => {
    const value = wire.sessionResult(await wire.matterCall('/api/matter/commissioning/cancel', { session: id }, signal()));
    assert.equal(wire.terminalSession(value), true);
    assert.equal(calls.filter(call => call.method === 'POST').length, 1);
  });
});

test('public capacity errors do not replay mutations', async () => {
  await peer(() => ({ status: 503, body: { ok: false, error: 'capacity' } }), async calls => {
    await assert.rejects(wire.matterCall('/api/thread/create', {}, signal()), /capacity/);
    assert.equal(calls.length, 1);
  });
});

test('service failure and mismatched job identities are rejected without replay', async () => {
  let mismatch = false;
  await peer(call => ({ body: call.method === 'POST' ? accepted : mismatch ? { ...completed(session), job: 2 } : { job: 1, state: 'complete', response: { schema: 1, ok: false, error: 'not_ready' } } }), async calls => {
    await assert.rejects(wire.matterCall('/api/thread/create', {}, signal()), /not_ready/);
    mismatch = true;
    await assert.rejects(wire.matterCall('/api/thread/create', {}, signal()), /protocol/);
    assert.equal(calls.filter(call => call.method === 'POST').length, 2);
  });
});

test('oversized responses and unknown error strings never reach diagnostics', async () => {
  let large = true;
  await peer(() => large ? { body: ' '.repeat(2049) } : { status: 500, body: { ok: false, error: 'PRIVATE_SENTINEL' } }, async () => {
    await assert.rejects(wire.matterAvailability(signal()), /protocol/);
    large = false;
    await assert.rejects(wire.matterAvailability(signal()), error => error.message === 'protocol');
  });
});

test('abort stops job polling while a submitted mutation is never replayed', async () => {
  const controller = new AbortController();
  await peer(call => {
    if (call.method === 'GET') setTimeout(() => controller.abort(), 20);
    return { body: call.method === 'POST' ? accepted : { job: 1, state: 'running' } };
  }, async calls => {
    await assert.rejects(wire.matterCall('/api/thread/create', {}, controller.signal), /cancelled/);
    const count = calls.length;
    await new Promise(resolve => setTimeout(resolve, 300));
    assert.equal(calls.length, count);
    assert.equal(calls.filter(call => call.method === 'POST').length, 1);
  });
});

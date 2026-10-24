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

test('status discovers running sessions and rejects malformed session IDs', async () => {
  const status = { enabled: true, available: true, controller_initialized: true, fabric_ready: true, state: 'running' };
  let current = status;
  await peer(call => ({ body: call.path === '/api/thread/status' ? thread : current }), async () => {
    assert.equal((await wire.matterAvailability(signal())).matter.commissioning_session, undefined);
    current = { ...status, commissioning_session: id };
    assert.equal((await wire.matterAvailability(signal())).matter.commissioning_session, id);
    for (const invalid of ['0000000000000000', id.toUpperCase(), '', 123, null]) {
      current = { ...status, commissioning_session: invalid };
      await assert.rejects(wire.matterAvailability(signal()), /protocol/);
    }
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

test('Thread retry reapplies stored credentials without requesting a dataset', async () => {
  await peer(call => ({ body: call.method === 'POST' ? accepted : completed(thread) }), async calls => {
    const result = await wire.matterCall('/api/thread/retry', {}, signal());
    assert.deepEqual(result, thread);
    assert.deepEqual(calls[0].body, { schema: 1 });
    await assert.rejects(wire.matterCall('/api/thread/retry', { dataset: '01020b00' }, signal()), /malformed/);
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

const device = { node: '0000000000000002', state: 'complete', online: true,
  id: 'matter:0000000000000001:0000000000000002:0001', endpoint: 1, servers: [6, 29] };
test('listing snapshot API is removed', async () => {
  await peer(() => ({ body: accepted }), async calls => {
    await assert.rejects(wire.matterCall('/api/matter/devices/snapshot', {}, signal()), /unsupported/);
    assert.equal(calls.length, 0);
    assert.equal(wire.matterDevicePage, undefined);
  });
});

test('OnOff reads and commands return only typed confirmed CASE values', async () => {
  let value = false;
  await peer(call => {
    if (call.method === 'POST') {
      if (call.path.endsWith('/command')) {
        if (call.body.command === 'on') value = true;
        if (call.body.command === 'off') value = false;
        if (call.body.command === 'toggle') value = !value;
      }
      return { body: accepted };
    }
    return { body: completed({ state: 'complete', job: '0000000000000001', result: {
      node: '0000000000000002', endpoint: 1, cluster: 6, attribute: 0, value: { type: 'bool', value } } }) };
  }, async calls => {
    assert.equal(await wire.matterOnOff('0000000000000002', 1, 'read', signal()), false);
    assert.equal(await wire.matterOnOff('0000000000000002', 1, 'on', signal()), true);
    assert.equal(await wire.matterOnOff('0000000000000002', 1, 'toggle', signal()), false);
    assert.deepEqual(calls.filter(call => call.method === 'POST').map(call => call.body), [
      { schema: 1, node: '0000000000000002', endpoint: 1, cluster: 6, attribute: 0 },
      { schema: 1, node: '0000000000000002', endpoint: 1, command: 'on' },
      { schema: 1, node: '0000000000000002', endpoint: 1, command: 'toggle' },
    ]);
  });
});

test('Boolean State reads return typed contact values', async () => {
  await peer(call => call.method === 'POST' ? { body: accepted } : { body: completed({ state: 'complete',
    job: '0000000000000001', result: { node: '0000000000000002', endpoint: 1, cluster: 69, attribute: 0,
      value: { type: 'bool', value: true } } }) }, async calls => {
    assert.equal(await wire.matterContact('0000000000000002', 1, signal()), true);
    assert.deepEqual(calls[0].body, { schema: 1, node: '0000000000000002', endpoint: 1, cluster: 69, attribute: 0 });
  });
});

test('unsupported or malformed device commands never reach the gateway', async () => {
  await peer(() => ({ body: accepted }), async calls => {
    await assert.rejects(wire.matterCall('/api/matter/devices/read', { node: id, endpoint: 1, cluster: 9, attribute: 0 }, signal()), /malformed/);
    await assert.rejects(wire.matterCall('/api/matter/devices/command', { node: id, endpoint: 65535, command: 'on' }, signal()), /malformed/);
    await assert.rejects(wire.matterCall('/api/matter/devices/command', { node: id, endpoint: 1, command: 'move_to_level', level: 255, transition_deciseconds: 0 }, signal()), /malformed/);
    await assert.rejects(wire.matterCall('/api/matter/devices/command', { node: id, endpoint: 1, command: 'move_to_level', level: 1, transition_deciseconds: 65536 }, signal()), /malformed/);
    assert.equal(calls.length, 0);
  });
});

test('Level reads and MoveToLevel return typed confirmed CurrentLevel values', async () => {
  let value = 18;
  await peer(call => {
    if (call.method === 'POST') {
      if (call.path.endsWith('/command')) value = call.body.level;
      return { body: accepted };
    }
    return { body: completed({ state: 'complete', job: '0000000000000001', result: {
      node: '0000000000000002', endpoint: 1, cluster: 8, attribute: 0, value: { type: 'u8', value } } }) };
  }, async calls => {
    assert.equal(await wire.matterLevel('0000000000000002', 1, { kind: 'read' }, signal()), 18);
    assert.equal(await wire.matterLevel('0000000000000002', 1,
      { kind: 'move', level: 124, transitionDeciseconds: 20 }, signal()), 124);
    assert.deepEqual(calls.filter(call => call.method === 'POST').map(call => call.body), [
      { schema: 1, node: '0000000000000002', endpoint: 1, cluster: 8, attribute: 0 },
      { schema: 1, node: '0000000000000002', endpoint: 1, command: 'move_to_level', level: 124, transition_deciseconds: 20 },
    ]);
  });
});

test('unknown CurrentLevel remains null and cannot confirm a MoveToLevel', async () => {
  await peer(call => call.method === 'POST' ? { body: accepted } : { body: completed({ state: 'complete',
    job: '0000000000000001', result: { node: '0000000000000002', endpoint: 1, cluster: 8, attribute: 0,
      value: { type: 'u8', value: null } } }) }, async () => {
    assert.equal(await wire.matterLevel('0000000000000002', 1, { kind: 'read' }, signal()), null);
    await assert.rejects(wire.matterLevel('0000000000000002', 1,
      { kind: 'move', level: 50, transitionDeciseconds: 0 }, signal()), /protocol/);
  });
});

test('Matter sensor reads retain signed values, units, quality and air-quality levels', async () => {
  const readings = new Map([
    [91, { type: 'air_quality', value: { value: 'moderate', quality: 'measured' } }],
    [1026, { type: 'measurement', value: { value: -5.25, unit: 'celsius', quality: 'measured' } }],
    [1029, { type: 'measurement', value: { value: null, unit: 'percent', quality: 'unknown' } }],
    [1037, { type: 'measurement', value: { value: 758.5, unit: 'ppm', quality: 'measured' } }],
    [1066, { type: 'measurement', value: { value: 12.75, unit: 'ug/m3', quality: 'measured' } }],
  ]);
  let requestedCluster = 0;
  await peer(call => {
    if (call.method === 'POST') { requestedCluster = call.body.cluster; return { body: accepted }; }
    return { body: completed({ state: 'complete', job: '0000000000000001', result: {
      node: '0000000000000002', endpoint: 1, cluster: requestedCluster,
      attribute: 0, value: readings.get(requestedCluster) } }) };
  }, async calls => {
    for (const cluster of [91, 1026, 1029, 1037, 1066])
      assert.deepEqual(await wire.matterSensor('0000000000000002', 1, cluster, signal()),
        cluster === 91 ? 'moderate' : readings.get(cluster).value);
    assert.equal(calls.filter(call => call.method === 'POST').length, 5);
  });
});

test('Matter endpoint adapter creates the shared device shape and drops descriptor-only endpoints', () => {
  const endpoint = { node: '0000000000000002', state: 'complete', online: true,
    id: 'matter:0000000000000001:0000000000000002:0001', endpoint: 1, servers: [6, 8, 29, 1026] };
  const device = wire.matterDeviceModel(endpoint);
  assert.equal(device.source, 'matter');
  assert.equal(device.id, endpoint.id);
  assert.equal(device.has_light, true);
  assert.equal(device.has_temperature, true);
  assert.equal(device.temperature_known, false);
  assert.equal(device.brightness_known, false);
  const contact = wire.matterDeviceModel({ ...endpoint, servers: [29, 69] }, { contact: true });
  assert.equal(contact.kind, 'contact');
  assert.equal(contact.has_contact, true);
  assert.equal(contact.contact_known, true);
  assert.equal(contact.contact_open, true);
  const measured = wire.matterDeviceModel({ ...endpoint, servers: [6, 8, 29, 91, 1026, 1029, 1037, 1066] }, {
    measurements: {
      91: 'moderate', 1026: { value: 21.5, unit: 'celsius', quality: 'measured' },
      1029: { value: null, unit: 'percent', quality: 'unknown' },
      1037: { value: 600, unit: 'ppm', quality: 'measured' },
      1066: { value: 7, unit: 'ug/m3', quality: 'measured' },
    },
  });
  assert.equal(measured.sensor_values.temperature.value, 21.5);
  assert.equal(measured.sensor_values.humidity.quality, 'unknown');
  assert.equal(measured.sensor_values.air_quality.value, 'moderate');
  assert.equal(measured.sensor_values.co2.value, 600);
  assert.equal(measured.sensor_values.pm25.value, 7);
  assert.equal(wire.matterDeviceModel({ ...endpoint, servers: [29] }), null);
});

test('subscription recovery paginates without replaying the recovery request', async () => {
  let cursor = 0;
  const row = { endpoint: 1, cluster: 1026, attribute: 0, id: 77, state: 'active', max_interval_seconds: 900,
    last_report_age_seconds: 3, retry_in_seconds: 907, failures: 0 };
  await peer(call => {
    if (call.method === 'POST') { cursor = call.body.cursor; return { body: accepted }; }
    return { body: completed({ node: id, intermittent: false, reachability: 'online',
      restore_scheduled: cursor === 0, subscriptions: [row], next_cursor: cursor === 0 ? 1 : null }) };
  }, async calls => {
    const value = await wire.matterSubscriptions(id, true, signal());
    assert.equal(value.subscriptions.length, 2);
    assert.equal(value.reachability, 'online');
    assert.deepEqual(calls.filter(call => call.method === 'POST').map(call => call.body), [
      { schema: 1, node: id, restore: true, cursor: 0 }, { schema: 1, node: id, restore: false, cursor: 1 },
    ]);
  });
});

test('subscription health rejects stale cursors and invalid report state', async () => {
  await peer(call => ({ body: call.method === 'POST' ? accepted : completed({ node: id, intermittent: true,
    reachability: 'sleeping', subscriptions: [], next_cursor: 0 }) }), async () => {
    await assert.rejects(wire.matterSubscriptions(id, false, signal()), /protocol/);
  });
  await assert.rejects(wire.matterCall('/api/matter/devices/subscriptions', { node: id, restore: true, cursor: 1 }, signal()), /malformed/);
});

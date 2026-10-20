const { test } = require('node:test');
const assert = require('node:assert/strict');
const { loadHistoryPages, historyWindow, mergeHistorySamples } = require(process.env.HISTORY_WEBUI_MODULE);
const sample = (cursor, timestamp = cursor, metric = 1, value = 20, unit = 'celsius') => [cursor, timestamp, metric, value, unit];
const response = (samples, next_before = 0) => ({ ok: true, json: async () => ({ samples, next_before }) });

async function withFetch(fetch, run) {
  const previous = global.fetch;
  global.fetch = fetch;
  try { await run(); } finally { global.fetch = previous; }
}

test('range pages are published before older requests finish; cursor and range travel together', async () => {
  const requests = [], snapshots = [];
  let finishOlder;
  await withFetch(async (url, options) => {
    requests.push({ url, signal: options.signal });
    return requests.length === 1 ? response([sample(101), sample(102)], 101) : new Promise(resolve => { finishOlder = resolve; });
  }, async () => {
    const controller = new AbortController();
    const pending = loadHistoryPages('matter:node:endpoint', 100, controller.signal, samples => snapshots.push(samples));
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(snapshots.length, 1);
    assert.deepEqual(snapshots[0].map(s => s[0]), [101, 102]);
    const first = new URL(requests[0].url, 'http://gateway');
    const second = new URL(requests[1].url, 'http://gateway');
    assert.match(first.pathname, /matter%3Anode%3Aendpoint/);
    assert.equal(first.searchParams.get('since'), '100');
    assert.equal(second.searchParams.get('since'), '100');
    assert.equal(second.searchParams.get('before'), '101');
    assert.equal(requests[0].signal, controller.signal);
    finishOlder(response([sample(99)]));
    assert.deepEqual((await pending).map(s => s[0]), [99, 101, 102]);
  });
});

test('cancellation discards an in-flight response even when the transport ignores abort', async () => {
  let reply, calls = 0;
  await withFetch(() => { calls++; return new Promise(resolve => { reply = resolve; }); }, async () => {
    const controller = new AbortController();
    const pending = loadHistoryPages('sensor', 100, controller.signal, () => assert.fail('stale page was published'));
    controller.abort();
    reply(response([sample(101)], 101));
    await assert.rejects(pending, { name: 'AbortError' });
    assert.equal(calls, 1);
  });
});

test('cancellation after a published page prevents further pagination', async () => {
  let calls = 0;
  await withFetch(async () => { calls++; return response([sample(101)], 101); }, async () => {
    const controller = new AbortController();
    await assert.rejects(loadHistoryPages('sensor', 100, controller.signal, () => controller.abort()), { name: 'AbortError' });
    assert.equal(calls, 1);
  });
});

test('a later failure leaves earlier published data available', async () => {
  let calls = 0;
  const snapshots = [];
  await withFetch(async () => {
    if (++calls === 1) return response([sample(101)], 101);
    return { ok: false, status: 500, json: async () => ({ error: 'older history unavailable' }) };
  }, async () => {
    await assert.rejects(loadHistoryPages('sensor', 100, new AbortController().signal, samples => snapshots.push(samples)), /older history unavailable/);
    assert.deepEqual(snapshots, [[sample(101)]]);
  });
});

test('all retained omits since, empty history completes, and the 10000 cap stops pagination', async () => {
  await withFetch(async url => {
    assert.equal(new URL(url, 'http://gateway').search, '');
    return response([]);
  }, async () => {
    assert.deepEqual(await loadHistoryPages('sensor', undefined, new AbortController().signal, () => {}), []);
  });
  let calls = 0;
  await withFetch(async () => {
    const top = 20000 - calls++ * 2000;
    return response(Array.from({ length: 2000 }, (_, i) => sample(top - 1999 + i)), top - 1999);
  }, async () => {
    const loaded = await loadHistoryPages('sensor', undefined, new AbortController().signal, () => {});
    assert.equal(loaded.length, 10000);
    assert.equal(calls, 5);
    assert.equal(loaded[0][0], 10001);
  });
});

test('refresh merging deduplicates cursors, and invalid pagination fails instead of looping', async () => {
  assert.deepEqual(mergeHistorySamples([sample(1), sample(2)], [sample(2), sample(3)]).map(s => s[0]), [1, 2, 3]);
  let calls = 0;
  await withFetch(async () => { calls++; return response([sample(100)], 100); }, async () => {
    await assert.rejects(loadHistoryPages('sensor', 0, new AbortController().signal, () => {}), /did not advance/);
    assert.equal(calls, 2);
  });
});

test('range clipping preserves the latest predecessor per metric and unit, without future samples', () => {
  const input = [sample(1, 10), sample(2, 20), sample(3, 25, 2, 50, 'percent'),
    sample(4, 30), sample(5, 40), sample(6, 21, 7, 1, 'ppm'), sample(7, 22, 7, 2, 'ug/m3')];
  const visible = historyWindow(input, 30, 35);
  assert.deepEqual(visible.map(s => [s[0], s[1]]), [[2, 30], [3, 30], [4, 30], [6, 30], [7, 30]]);
  assert.equal(input[1][1], 20);
  assert.deepEqual(historyWindow([], 30, 35), []);
});

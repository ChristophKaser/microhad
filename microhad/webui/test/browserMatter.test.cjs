const { test } = require('node:test');
const assert = require('node:assert/strict');
const scanner = require(process.env.BROWSER_MATTER_WEBUI_MODULE);

test('Matter QR payloads are accepted locally and bounded before use', () => {
  assert.equal(scanner.matterQrCode('MT:Y.K9042C00KA0648G00'), true);
  assert.equal(scanner.matterQrCode('https://matter.ikea.com/v1/add/?r=E2495%2CMT%3AEX7A04Y614EEGC1CA10'), true);
  assert.equal(scanner.matterQrCode('https://vendor.example/pair?setup=MT%3AY.K9042C00KA0648G00'), true);
  assert.equal(scanner.matterQrCode('https://example.test/not-matter'), false);
  assert.equal(scanner.matterQrCode('M'.repeat(513)), false);
});

test('camera scanning requires a secure browser with detector and camera APIs', () => {
  const savedWindow = global.window, savedNavigator = global.navigator;
  try {
    global.window = { isSecureContext: true, BarcodeDetector: class {} };
    global.navigator = { mediaDevices: { getUserMedia: async () => ({}) } };
    assert.equal(scanner.webQrScannerAvailable(), true);
    global.window.isSecureContext = false;
    assert.equal(scanner.webQrScannerAvailable(), false);
    global.window.isSecureContext = true; delete global.navigator.mediaDevices;
    assert.equal(scanner.webQrScannerAvailable(), false);
  } finally { global.window = savedWindow; global.navigator = savedNavigator; }
});

for (const indicationReplyFirst of [false, true]) {
  test(`BLE indications have independent sequence numbers with overlapping replies (indication first: ${indicationReplyFirst})`, async () => {
    const saved = { window: global.window, location: global.location, WebSocket: global.WebSocket, crypto: global.crypto };
    const timers = new Map(), requests = [], events = [], failures = [], progress = [];
    let timerId = 0, socket, notify, confirm;
    const reply = (request, result = {}) => socket.onmessage({ data: JSON.stringify({ id: request.id, response: { ok: true, result } }) });
    class Socket {
      static OPEN = 1;
      readyState = 1;
      constructor() { socket = this; queueMicrotask(() => this.onopen()); }
      send(raw) {
        const request = JSON.parse(raw);
        requests.push(request);
        if (request.opcode === 4) queueMicrotask(() => reply(request));
        if (request.opcode === 6) queueMicrotask(() => reply(request, { generation: 14 }));
      }
      close() { this.readyState = 3; this.onclose({ code: 1000, wasClean: true }); }
    }
    const indicate = () => notify({ target: { value: new DataView(Uint8Array.from([0x65, 0x6c, 4, 0xf4, 0, 5]).buffer) } });
    const c1 = { writeValueWithResponse: async () => { events.push('C1'); await new Promise(resolve => { confirm = resolve }); events.push('confirmed'); } };
    const c2 = {
      addEventListener: (_, listener) => { notify = listener; },
      startNotifications: async () => { events.push('subscribe'); indicate(); }
    };
    const device = {
      gatt: { connect: async () => ({ getPrimaryService: async () => ({ getCharacteristic: async id => id.endsWith('11') ? c1 : c2 }) }) },
      addEventListener() {}
    };
    let relay;
    try {
      global.window = { setTimeout: callback => { timers.set(++timerId, callback); return timerId; }, clearTimeout: id => timers.delete(id) };
      global.location = { protocol: 'https:', host: 'gateway.test' };
      global.WebSocket = Socket;
      global.crypto = require('node:crypto').webcrypto;
      relay = new scanner.BrowserMatterRelay('0000000000000001', value => progress.push(value), error => failures.push(error));
      await relay.connect(device);
      const handler = socket.onmessage;
      socket.onmessage({ data: JSON.stringify({ event: 'ble_write', data: { generation: 14, write: 1, data: '656c04000000f70006' } }) });
      await Promise.resolve();
      assert.deepEqual(events, ['C1']);
      confirm();
      await new Promise(resolve => setImmediate(resolve));
      assert.deepEqual(events, ['C1', 'confirmed', 'subscribe']);
      const indication = requests.find(request => request.opcode === 7);
      const ack = requests.find(request => request.opcode === 8);
      assert.equal(indication.body.sequence, 1);
      assert.equal(indication.body.generation, 14);
      assert.notEqual(indication.id, indication.body.sequence);
      indicate(); // Reserve the next BLE sequence even before the first reply.
      const second = requests.filter(request => request.opcode === 7)[1];
      assert.equal(second.body.sequence, 2);
      const replies = indicationReplyFirst ? [indication, ack] : [ack, indication];
      for (const request of replies) reply(request);
      reply(second);
      await relay.write;
      await Promise.resolve();
      assert.equal(socket.onmessage, handler);
      assert.equal(timers.size, 0);
      assert.deepEqual(failures, []);
      socket.onmessage({ data: JSON.stringify({ event: 'commissioning', data: { state: 'pase' } }) });
      assert.equal(progress.length, 1);

      const pending = relay.call(8, { generation: 14, write: 2, success: true });
      relay.close();
      await assert.rejects(pending, { message: 'transport' });
      assert.equal(timers.size, 0);
    } finally {
      relay?.close();
      Object.assign(global, saved);
    }
  });
}

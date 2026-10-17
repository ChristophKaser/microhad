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

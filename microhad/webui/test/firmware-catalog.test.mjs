import { test } from 'node:test'
import assert from 'node:assert/strict'
import { build } from 'esbuild'
const bundle = await build({ entryPoints: [new URL('../src/firmwareCatalog.ts', import.meta.url).pathname], bundle: true, write: false, platform: 'node', format: 'esm' })
const { parseCatalog, matchingFirmware, firmwareUrl } = await import(`data:text/javascript;base64,${Buffer.from(bundle.outputFiles[0].text).toString('base64')}`)
const community = { id: 'zigbee-ota', label: 'zigbee-OTA' }
const sonoff = { id: 'sonoff', label: 'SONOFF', manufacturer: 4742 }
const ikea = { id: 'ikea', label: 'IKEA', manufacturer: 4476 }
const device = { source: 'zigbee', manufacturer_name: 'SONOFF', model_id: 'TRVZB', ota_manufacturer: 4742, ota_image_type: 8199, ota_file_version: 1 }
const row = { manufacturerCode: 4742, imageType: 8199, fileVersion: 2, fileSize: 256, url: 'https://example.test/image.ota' }
test('matches numeric OTA identity and sorts versions', () => {
  const images = parseCatalog(community, [row, { ...row, fileVersion: 3 }])
  assert.deepEqual(matchingFirmware(device, images).map(i => i.version), [3, 2])
  assert.equal(matchingFirmware({ ...device, ota_image_type: 8200 }, images).length, 0)
  assert.equal(matchingFirmware({ ...device, ota_manufacturer: 1 }, images).length, 0)
  assert.equal(matchingFirmware({ ...device, source: 'matter' }, images).length, 0)
})
test('requires metadata restrictions to be verifiable', () => {
  for (const restriction of [{ modelId: 'other' }, { manufacturerName: ['other'] }, { hardwareVersionMin: 1 }, { hardwareVersionMax: 3 }, { minFileVersion: 2 }, { maxFileVersion: 0 }]) {
    assert.equal(matchingFirmware(device, parseCatalog(community, [{ ...row, ...restriction }])).length, 0)
  }
  assert.equal(matchingFirmware({ ...device, hardware_version: 2 }, parseCatalog(community, [{ ...row, hardwareVersionMin: 1, hardwareVersionMax: 3 }])).length, 1)
})
test('requires OTA image type even when the model matches', () => {
  const unknown = { ...device, ota_manufacturer: null, ota_image_type: null, node_descriptor: { manufacturer_code: 4742 } }
  assert.equal(matchingFirmware(unknown, parseCatalog(community, [row])).length, 0)
  assert.equal(matchingFirmware(unknown, parseCatalog(community, [{ ...row, modelId: 'TRVZB' }])).length, 0)
})
test('normalizes SONOFF and prefers it over duplicate community entries', () => {
  const images = parseCatalog(sonoff, [{ fw_manufacturer_id: 4742, fw_image_type: 8199, fw_file_version: 2, model_id: 'TRVZB', fw_binary_url: row.url }])
  assert.equal(images[0].source, 'SONOFF')
  assert.equal(matchingFirmware(device, [...images, ...parseCatalog(community, [row])]).length, 1)
  assert.equal(matchingFirmware(device, [...images, ...parseCatalog(community, [row])])[0].source, 'SONOFF')
})
test('normalizes current and legacy IKEA metadata, preserving original download URLs', () => {
  const legacy = { fw_type: 2, fw_manufacturer_id: 4476, fw_image_type: 100, fw_file_version_MSB: 2, fw_file_version_LSB: 1, fw_binary_url: 'http://example.test/original.bin' }
  const modern = { fw_image_type: 101, fw_sha3_256: 'abc', fw_binary_url: 'https://example.test/image_v123_ota.ota' }
  const images = parseCatalog(ikea, [legacy, modern, { ...legacy, fw_type: 1 }, { ...modern, fw_image_type: 8704 }])
  assert.equal(images.length, 2)
  assert.equal(images[0].version, 131073)
  assert.equal(images[0].url, legacy.fw_binary_url)
  assert.equal(images[1].version, 123)
})
test('rejects unsafe URLs, malformed and out-of-range metadata', () => {
  for (const url of ['javascript:alert(1)', 'data:text/plain,test', 'https://user:password@example.test/file', '/relative']) assert.equal(firmwareUrl(url), undefined)
  for (const bad of [{ url: 'javascript:alert(1)' }, { fileVersion: -1 }, { fileVersion: 2 ** 32 }, { manufacturerCode: 65536 }, { minFileVersion: '2' }]) assert.equal(parseCatalog(community, [{ ...row, ...bad }]).length, 0)
  assert.throws(() => parseCatalog(community, {}))
})

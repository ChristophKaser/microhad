import { test } from 'node:test'
import assert from 'node:assert/strict'
import { build } from 'esbuild'
const bundle = await build({ entryPoints: [new URL('../src/MatterFirmwareDownloads.tsx', import.meta.url).pathname], bundle: true, write: false, platform: 'node', format: 'esm', jsx: 'automatic' })
const { parseMatterVersion } = await import(`data:text/javascript;base64,${Buffer.from(bundle.outputFiles[0].text).toString('base64')}`)
const row = { vid: 4476, pid: 4096, softwareVersion: 17039372, softwareVersionString: '1.4.12', softwareVersionValid: true, otaUrl: 'https://example.test/fw.ota', minApplicableSoftwareVersion: 17039360, maxApplicableSoftwareVersion: 17039371 }
const parse = (v, installed = 17039366) => parseMatterVersion({ modelVersion: v }, 4476, 4096, installed, 17039372, 'Matter testnet', 2)
test('accepts applicable Matter testnet firmware', () => { assert.equal(parse(row).label, '1.4.12'); assert.equal(parse(row).channel, 'Matter testnet') })
test('rejects wrong identity, invalid, non-upgrade, and incompatible images', () => {
  for (const change of [{ vid: 1 }, { pid: 1 }, { softwareVersion: 1 }, { softwareVersionValid: false }, { otaUrl: 'javascript:alert(1)' }, { minApplicableSoftwareVersion: 17039367 }, { maxApplicableSoftwareVersion: 17039365 }, { minHardwareVersion: 3 }, { maxHardwareVersion: 1 }]) assert.equal(parse({ ...row, ...change }), undefined)
  assert.equal(parse(row, 17039372), undefined)
})

import { strict as assert } from 'node:assert'
import { readFileSync } from 'node:fs'
import test from 'node:test'
import { transform } from 'esbuild'

const source = readFileSync(new URL('../src/scenes.ts', import.meta.url), 'utf8')
const { code: js } = await transform(source, { loader: 'ts', format: 'esm' })
const { newState, parseScenes, serializeScenes } = await import(`data:text/javascript;base64,${Buffer.from(js).toString('base64')}`)

test('Matter group members and button positions survive scene configuration', () => {
  const devices = [
    { source: 'zigbee', ieee_address: 'zigbee-light', external_id: '', has_brightness: true, brightness_percent: 50, light_on: true, endpoint_descriptors: [] },
    { source: 'matter', ieee_address: '', external_id: 'matter-light', has_brightness: true, brightness_percent: 30, light_on: true, endpoint_descriptors: [] },
    { source: 'matter', ieee_address: '', external_id: 'matter-remote' },
  ]
  const config = {
    groups: [{ id: 1, name: 'Room', members: ['zigbee-light', 'matter-light'], scenes: [
      { id: 1, name: 'Evening', states: [newState('zigbee-light', devices), newState('matter-light', devices)] },
    ] }],
    bindings: [{ ieee: 'matter-remote', action: 'matter_hold', groupId: 1, off: false, dimDirection: -1, position: 2, sceneIds: [] }],
    nativeBindings: [],
  }
  const restored = parseScenes(serializeScenes(config, devices))
  assert.deepEqual(restored, config)
  assert.equal(newState('matter-light', devices).brightness, 30)
})

test('existing scene bindings retain any-button behavior', () => {
  const restored = parseScenes('B\tremote\ton\t1\t0\t2\n')
  assert.equal(restored.bindings[0].position, -1)
  assert.equal(restored.bindings[0].dimDirection, 0)
})

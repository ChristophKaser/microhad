import { test } from 'node:test'
import assert from 'node:assert/strict'
import { buildSync } from 'esbuild'
import { fileURLToPath } from 'node:url'
import Module from 'node:module'
const load = name => {
  const result = buildSync({ entryPoints: [fileURLToPath(new URL(`../src/${name}.ts`, import.meta.url))], bundle: true, platform: 'node', format: 'cjs', write: false })
  const module = new Module(name)
  module._compile(result.outputFiles[0].text, `${name}.cjs`)
  return module.exports
}
const { electricalDefinitions } = load('electrical')
const { historyName, historyUnit, historyY, historyLabel } = load('history')
test('electrical histories preserve units, signs, and existing watts identity', () => {
  assert.equal(new Set(electricalDefinitions.map(d => d.metric)).size, 25)
  for (const d of electricalDefinitions) {
    const sample = [1, 100, d.metric, -12.345, d.metric === 4 ? 'watts' : d.unit]
    assert.equal(historyY(d.metric, sample), -12.345)
    assert.equal(historyUnit(d.metric, sample), d.unit)
    assert.ok(historyName(d.metric))
    assert.ok(historyLabel(d.metric, sample).includes('-12.3'))
  }
  assert.equal(electricalDefinitions.find(d => d.key === 'active_power').metric, 4)
  assert.equal(historyName(4), 'Power')
})
test('energy totals retain their normalized value without legacy scaling', () => {
  const energy = electricalDefinitions.find(d => d.key === 'cumulative_imported_energy')
  assert.equal(historyY(energy.metric, [1, 100, energy.metric, 123.456789, 'kWh']), 123.456789)
  assert.equal(historyUnit(energy.metric), 'kWh')
})

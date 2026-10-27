import { test } from 'node:test'
import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import { _steppedLineTo } from 'chart.js/helpers'

for (const component of ['HistoryDialog', 'Dashboard']) {
  test(`${component} holds the recorded contact state until the next report`, () => {
    const source = readFileSync(new URL(`../src/${component}.tsx`, import.meta.url), 'utf8')
    const mode = source.match(/stepped: discrete \? '([^']+)' : false/)?.[1]
    assert.ok(mode, 'discrete chart step mode is configured')
    // Closed for 12 hours, then open for one minute, then closed again.
    const samples = [{ x: 0, y: 0 }, { x: 43200, y: 1 }, { x: 43260, y: 0 }]
    const path = [samples[0]]
    const context = { lineTo: (x, y) => path.push({ x, y }) }
    for (let i = 1; i < samples.length; i++) {
      _steppedLineTo(context, samples[i - 1], samples[i], false, mode)
    }
    let openSeconds = 0
    for (let i = 1; i < path.length; i++) {
      if (path[i - 1].y === 1) openSeconds += path[i].x - path[i - 1].x
    }
    assert.equal(openSeconds, 60)
    assert.deepEqual(path[1], { x: 43200, y: 0 })
    assert.deepEqual(path[3], { x: 43260, y: 1 })
  })
}

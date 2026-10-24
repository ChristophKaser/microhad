import { test } from 'node:test'
import assert from 'node:assert/strict'
import { readFileSync, mkdtempSync, writeFileSync, rmSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { spawnSync } from 'node:child_process'
import vm from 'node:vm'
import { build } from 'esbuild'
import { patchLuaWorker, generateLuaWorker } from '../scripts/lua-worker.mjs'

const output = await build({ entryPoints: [new URL('../src/automationCompletion.ts', import.meta.url).pathname], bundle: true, format: 'esm', write: false })
const { completions, gatewayApi, argumentTypes, eventNames, filterKeys, snapshotFields, remoteActions } = await import('data:text/javascript;base64,' + Buffer.from(output.outputFiles[0].text).toString('base64'))
const device = { ieee_address: '0x123', id: 'dashboard-id', name: 'Desk "lamp"', kind: 'light', source: 'zigbee', has_light: true, has_brightness: true, endpoint_descriptors: [{ endpoint: 1 }] }
const catalog = { devices: [device, { ...device, ieee_address: '0x456', name: 'Switch', source: 'matter', has_light: false, has_actor: true }], groups: [{ id: 7, name: 'Living room', scenes: [{ id: 2, name: 'Evening' }] }] }
const suggest = (code, data = catalog) => completions(code, code.length, data)
const values = code => suggest(code).map(item => item.value)

test('gateway APIs, snippets and sandbox globals', () => {
  assert(values('devices.').includes('light'))
  assert(values('local x = { nested = devices.').includes('light'))
  assert(suggest('devices.li').find(item => item.value === 'light').snippet.includes('${1:target}'))
  assert(values('automation.').includes('cron'))
  assert.deepEqual(values('devices.actor("0x123", '), ['true', 'false'])
  assert(!values('').includes('os'))
  assert(!values('').includes('require'))
  assert(values('math.').includes('floor'))
  assert(!values('math.').includes('execute'))
  assert(values('devices.list({ ').includes('online = '))
  assert(values('devices.light("0x123", { ').includes('brightness = '))
  assert.deepEqual(values('devices.light("0x123", { on = '), ['true', 'false'])
  assert(values('notify.send("hello", "world", { ').includes('device_id = '))
  assert(values('notify.send("hello", "world", { ').includes('recipients = '))
  assert(values('log("w').includes('warn'))
})

test('device IDs, unambiguous names, capabilities, refreshed catalogs and strings', () => {
  const suggestions = suggest('devices.light("')
  assert(suggestions.some(item => item.value === '0x123'))
  assert(!suggestions.some(item => item.value === 'dashboard-id'))
  assert(suggestions.find(item => item.value === '0x123').score > suggestions.find(item => item.value === '0x456').score)
  assert(values('devices.get("Desk ').includes('Desk "lamp"'))
  assert(values('devices.get(').includes('"Desk \\"lamp\\""'.replace(/\\\\/g, '\\')))
  assert(!suggest('devices.get("', { ...catalog, devices: [device, { ...device, ieee_address: 'other' }] }).some(item => item.value === device.name))
  assert(!values('events.on({ device_id = "').includes(device.name))
  assert(suggest('devices.get("', { devices: [], groups: [] }).length === 0)
  assert(!suggest('devices.get("', { ...catalog, devices: [{ ...device, name: 'Renamed' }] }).some(item => item.value === device.name))
  assert(values('scenes.activate("7", "').includes('Evening'))
  assert(values('scenes.off("').includes('7'))
  assert.deepEqual(values('-- devices.'), [])
  assert.deepEqual(values('--[[ devices.'), [])
  assert.deepEqual(values('local x = [[devices.'), [])
  assert.deepEqual(values('local x = "devices.'), [])
})

test('event names, filter arguments, callback names and payload fields', () => {
  assert(values('events.on({ type = "device.').includes('device.changed'))
  assert(values('events.wait_for({ ').includes('type = '))
  assert(values('events.on({ type = "remote.action", action = "matter_').includes('matter_press'))
  assert(!values('events.on({ type = "device.value", ').includes('action = '))
  assert(values('events.on({ device = "0x123", field = "').includes('brightness_percent'))
  assert(!values('events.on({ device = "0x123", field = "').includes('contact_open'))
  assert(values('events.on({ type = "remote.action" }, function(button) button.').includes('press_count'))
  assert(values('events.on({ type = "device.changed" }, function(change) change.device.').includes('name'))
  assert(!values('events.on({ type = "device.online" }, function(change) change.').includes('old'))
  assert.deepEqual(values('events.on({ type = "device.online" }, function(change) end) change.'), [])
  assert(values('local x = { nested = devices.get("').includes('0x123'))
  assert.deepEqual(values('events.on({ type = "timer.fired" }, function(event) event.device.'), [])
})

test('completion metadata matches actual runtime contracts', () => {
  const runtime = readFileSync(new URL('../../automation/runtime.lua', import.meta.url), 'utf8')
  const native = readFileSync(new URL('../../src/automation_worker.cpp', import.meta.url), 'utf8')
  const snapshot = readFileSync(new URL('../../src/automation_types.cpp', import.meta.url), 'utf8')
  const gateway = readFileSync(new URL('../../src/gateway.cpp', import.meta.url), 'utf8')
  assert.deepEqual(new Set(eventNames), new Set([...runtime.matchAll(/\['([^']+)'\]=true/g)].map(m => m[1])))
  const allowed = runtime.match(/local allowed=\{([^}]+)\}/)[1]
  assert.deepEqual(new Set(filterKeys), new Set([...allowed.matchAll(/(\w+)=true/g)].map(m => m[1])))
  for (const field of snapshotFields) assert(snapshot.includes('"' + field + '"'), field)
  for (const [name, spec] of Object.entries(gatewayApi)) {
    for (const argument of spec[0].split(', ').filter(Boolean)) assert(argumentTypes[argument], name + ': ' + argument)
    const [namespace, member] = name.split('.')
    assert(member ? runtime.includes(name) || runtime.includes(member + '=') || runtime.includes(member + ' =') : runtime.includes('env.' + namespace + '='), name)
  }
  for (const action of remoteActions) assert(gateway.includes('"' + action + '"'), action)
  assert(snapshot.includes('ambiguous name; use stable IDs'))
  assert(native.includes('"recipients"'))
})

const workerSource = readFileSync(new URL('../node_modules/ace-builds/src/worker-lua.js', import.meta.url), 'utf8')
function parser(source) {
  const context = vm.createContext({ console, postMessage() {}, setTimeout, clearTimeout })
  vm.runInContext(source, context)
  const { Worker } = context.require('ace/mode/lua_worker')
  return new Worker({ on() {}, emit() {} }).parser
}
const lua = parser(patchLuaWorker(workerSource))
export const fixtures = [
  ['global x = 1', true],
  ['global <const> x, y = 1, 2', true],
  ['global x <const> = 1', true],
  ['global *', true],
  ['global <const> *', true],
  ['global function f(...args) return args[1], ... end', true],
  ['local function f(x, ...args) return args[1] end', true],
  ['local <const> x, y = 1, 2', true],
  ['local <close> x = nil', true],
  ['local <close> x, y = nil, nil', false],
  ['global x <close> = nil', false],
  ['global <close> *', false],
  ['global <unknown> x', false],
  ['global function f.a() end', false],
  ['local function f(...args, x) end', false],
  ['local global = 1; global = global + 1', true],
  ['local x <const> = 1', true],
  ['local x <close> = nil', true],
  ['local a <const>, b <close> = 1, nil', true],
  ['local x <const> = 1; do local y <close> = nil end', true],
  ['local x <unknown> = 1', false],
  ['local x <const = 1', false],
  ['local x <> = 1', false],
  ['local x <const> <close> = 1', false],
  ['local a <close>, b <close> = nil, nil', false],
  ['local x = "<const>" -- <close>\nreturn x', true],
  ['local s = "\\u{10ffff}\\u{110000}\\u{7fffffff}"', true],
  ['local s = "\\u{80000000}"', false],
  ['local s = "\\u{000000007fffffff}"', true],
  ['local s = "\\u{xyz}"', false],
  ['if true then', false],
  ['local x = (2 + 3) // 2; return x << 1', true],
]
test('Lua 5.5 parser accepts attributes and expanded escapes without changing positions', async () => {
  for (const [source, valid] of fixtures) {
    if (valid) assert.doesNotThrow(() => lua.parse(source), source)
    else assert.throws(() => lua.parse(source), undefined, source)
  }
  try { lua.parse('-- unchanged\nlocal x <wat> = 1'); assert.fail('expected error') }
  catch (failure) { assert.equal(failure.line, 2); assert.equal(failure.column, 9) }
  assert.doesNotThrow(() => patchLuaWorker(workerSource + '\n'))
  assert.throws(() => patchLuaWorker(workerSource.replace('  function parseLocalStatement(flowContext) {', '  function renamedLocalStatement(flowContext) {')))
  await generateLuaWorker()
  const minified = parser(readFileSync(new URL('../node_modules/.cache/microhad-worker-lua.js', import.meta.url), 'utf8'))
  for (const [source, valid] of fixtures) assert.equal((() => { try { minified.parse(source); return true } catch { return false } })(), valid, source)
})

test('patched worker fixtures agree with the repository Lua 5.5.1 compiler', () => {
  const directory = mkdtempSync(join(tmpdir(), 'automation-lua-syntax-'))
  try {
    const root = new URL('../../dependencies/lua/', import.meta.url).pathname
    const makefile = readFileSync(new URL('../../Makefile', import.meta.url), 'utf8')
    const names = makefile.match(/^LUA_NAMES := (.+)$/m)[1].split(' ')
    const executable = join(directory, 'lua-syntax')
    const compile = spawnSync('cc', ['-O2', '-I' + root, new URL('lua-syntax.c', import.meta.url).pathname, ...names.map(name => root + name + '.c'), '-lm', '-o', executable], { encoding: 'utf8' })
    assert.equal(compile.status, 0, compile.stderr)
    for (const [source, valid] of fixtures) {
      const filename = join(directory, 'fixture.lua')
      writeFileSync(filename, source)
      const checked = spawnSync(executable, [filename], { encoding: 'utf8' })
      assert.equal(checked.status, valid ? 0 : 1, source + ': ' + checked.stderr)
    }
  } finally { rmSync(directory, { recursive: true, force: true }) }
})

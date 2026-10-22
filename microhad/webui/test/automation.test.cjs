const { test } = require('node:test')
const assert = require('node:assert/strict')
const path = require('node:path')
const Module = require('node:module')
const build = process.env.AUTOMATION_WEBUI_BUILD
const api = require(path.join(build, 'automation.js'))

test('automation names, disabled suffixes, log isolation, severity, time and clear cursor', () => {
  assert(api.validAutomationFilename('office-temperature.lua'))
  assert(api.validAutomationFilename('office.lua.disabled'))
  for (const name of ['../x.lua', 'a/b.lua', '.lua', 'x.lua.next', 'x'.repeat(97) + '.lua']) assert(!api.validAutomationFilename(name))
  assert.equal(api.disabledFilename('office.lua', false), 'office.lua.disabled')
  assert.equal(api.disabledFilename('office.lua.disabled', true), 'office.lua')
  const records = [{ record_id: 1, script_id: 'a', level: 'warn', at: 100 }, { record_id: 2, script_id: 'b', level: 'warn', at: 110 }, { record_id: 3, script_id: 'a', level: 'error', at: 120 }]
  assert.deepEqual(api.visibleAutomationLogs(records, 'a', 'warn', 0, 0), [records[0]])
  assert.deepEqual(api.visibleAutomationLogs(records, 'a', '', 110, 1), [records[2]])
  assert.deepEqual(api.visibleAutomationLogs(records, 'a', '', 0, 3), [])
})

test('editor creates drafts, validates without saving, saves, protects edits and controls lifecycle', async () => {
  const states = [], effects = [], cleanup = [], browserListeners = new Map()
  let cursor = 0, prompt = 'office.lua', confirm = true, dirty = false
  const hooks = {
    useState(initial) { const at = cursor++; if (!(at in states)) states[at] = initial; return [states[at], value => { states[at] = typeof value === 'function' ? value(states[at]) : value }] },
    useRef(value) { const at = cursor++; if (!(at in states)) states[at] = { current: value }; return states[at] },
    useCallback(fn) { return fn },
    useEffect(fn) { effects.push(fn) },
  }
  const originalLoad = Module._load
  Module._load = function (request, parent, main) {
    if (request === 'react') return hooks
    if (request === 'react/jsx-runtime') return { jsx: (type, props) => ({ type, props }), jsxs: (type, props) => ({ type, props }) }
    return originalLoad.call(this, request, parent, main)
  }
  const { AutomationScripts } = require(path.join(build, 'AutomationScripts.js'))
  Module._load = originalLoad
  global.window = { prompt: () => prompt, confirm: () => confirm, setInterval: () => 1, clearInterval() {}, addEventListener: (name, fn) => browserListeners.set(name, fn), removeEventListener: name => browserListeners.delete(name) }
  let copied
  global.navigator = { clipboard: { writeText: async text => { copied = text } } }
  const calls = [], scripts = [], sources = new Map()
  global.fetch = async (url, options = {}) => {
    calls.push([url, options])
    let value
    if (url === '/api/automations') value = { scripts: scripts.map(s => ({ ...s })) }
    else if (url === '/api/automations/logs') value = { records: [{ record_id: 1, script_id: 'office', generation: 1, at: 100, level: 'warn', operation: 'log', message: 'test warning', fields: {} }] }
    else if (!options.method) value = { source: sources.get(decodeURIComponent(url.split('/').pop())) }
    else {
      const fields = Object.fromEntries(new URLSearchParams(options.body)); const action = url.split('/').pop()
      if (action === 'save') { sources.set(fields.filename, fields.source); if (!scripts.some(s => s.filename === fields.filename)) scripts.push({ id: 'office', filename: fields.filename, enabled: true, loaded: true, generation: 1, listeners: 0, timers: 0, coroutines: 0 }) }
      if (action === 'disable') scripts[0].filename = api.disabledFilename(fields.filename, false), scripts[0].enabled = false
      if (action === 'enable') scripts[0].filename = api.disabledFilename(fields.filename, true), scripts[0].enabled = true
      if (action === 'delete') scripts.splice(0)
      value = { ok: true }
    }
    return { ok: true, json: async () => value }
  }
  let tree
  const render = async () => {
    for (const dispose of cleanup.splice(0)) dispose?.()
    cursor = 0; effects.length = 0
    tree = AutomationScripts({ onDirtyChange: value => { dirty = value } })
    for (const effect of effects.splice(0)) cleanup.push(effect())
    await new Promise(resolve => setImmediate(resolve))
  }
  const walk = node => Array.isArray(node) ? node.flatMap(walk) : node && typeof node === 'object' ? [node, ...walk(node.props?.children)] : []
  const nodes = () => walk(tree)
  const text = node => typeof node === 'string' || typeof node === 'number' ? String(node) : Array.isArray(node) ? node.map(text).join('') : node ? text(node.props?.children) : ''
  const button = label => { const found = nodes().find(node => node.type === 'button' && text(node) === label); assert(found, label); return found }
  const click = async label => { button(label).props.onClick(); await new Promise(resolve => setImmediate(resolve)); await render() }
  await render(); await click('New script')
  assert(dirty); assert(browserListeners.has('beforeunload'))
  let editor = nodes().find(node => node.type === 'textarea'); editor.props.onChange({ target: { value: 'persistent.count = 1' } }); await render()
  await click('Validate'); assert(!calls.some(([url]) => url.endsWith('/save'))); assert(dirty)
  await click('Save'); assert(!dirty); assert.equal(sources.get('office.lua'), 'persistent.count = 1')
  const body = new URLSearchParams(calls.find(([url]) => url.endsWith('/save'))[1].body); assert.equal(body.get('reload'), '0')
  await click('Save and reload'); assert.equal(new URLSearchParams(calls.filter(([url]) => url.endsWith('/save')).at(-1)[1].body).get('reload'), '1')
  await click('Copy record'); assert.match(copied, /test warning/)
  await click('Clear displayed logs'); assert(!nodes().some(node => text(node) === 'test warning'))
  editor = nodes().find(node => node.type === 'textarea'); editor.props.onChange({ target: { value: 'unsaved' } }); await render(); confirm = false
  await click('New script'); assert.equal(nodes().find(node => node.type === 'textarea').props.value, 'unsaved')
  const resetsBefore = calls.filter(([url]) => url.endsWith('/reset')).length
  await click('Reset persistent state'); assert.equal(calls.filter(([url]) => url.endsWith('/reset')).length, resetsBefore)
  confirm = true; await click('Disable'); assert.equal(scripts[0].filename, 'office.lua.disabled'); assert(dirty)
  await click('Enable'); assert.equal(scripts[0].filename, 'office.lua')
  await click('Reset persistent state'); await click('Delete'); assert.equal(scripts.length, 0); assert(!dirty)
  for (const dispose of cleanup) dispose?.()
})

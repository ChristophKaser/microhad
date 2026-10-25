import type { Device, SceneGroupSummary } from './api'

export interface Completion { stringStart?: number; delimiter?: string; caption: string; value: string; meta: string; score: number; docText?: string; snippet?: string }
export interface Catalog { devices: Device[]; groups: SceneGroupSummary[] }
type Spec = [signature: string, returns: string, description: string]
export const gatewayApi: Record<string, Spec> = {
  'triggers.register': ['id, label, callback', 'nil', 'Register or replace a manual trigger; dashboard placement survives unload and reload.'],
  'triggers.unregister': ['id', 'nil', 'Remove a manual trigger handler; saved dashboard buttons remain.'],
  'events.on': ['filter, callback', 'handle', 'Subscribe to matching events; callback receives an immutable event.'],
  'events.off': ['handle', 'nil', 'Remove an event listener.'],
  'events.wait_for': ['filter, timeout', 'event | nil, error', 'Wait inside a callback; timeout is optional seconds.'],
  'automation.now': ['', 'number', 'Current Unix time.'],
  'automation.script_id': ['', 'string', 'Current script identifier.'],
  'automation.after': ['seconds, callback, payload', 'handle', 'Run once; callback receives optional JSON-compatible payload.'],
  'automation.every': ['seconds, callback, payload', 'handle', 'Run repeatedly; callback receives optional payload.'],
  'automation.cancel': ['handle', 'boolean', 'Cancel a timer or suspended callback.'],
  'automation.sleep': ['seconds', 'nil', 'Suspend the current callback.'],
  'automation.cron': ['expression, callback', 'handle', 'Schedule using five cron fields: minute hour day month weekday.'],
  'devices.get': ['target', 'device | nil, error', 'Read an immutable device snapshot by stable ID or unique name.'],
  'devices.list': ['filter', 'device[]', 'List snapshots; optional filter supports source, kind, online, name.'],
  'devices.actor': ['target, on', 'boolean | nil, error', 'Set an actor using a boolean.'],
  'devices.light': ['target, state', 'boolean | nil, error', 'Set on and/or brightness (integer 0–100).'],
  'devices.thermostat': ['target, field, value', 'boolean | nil, error', 'Set one thermostat field using a string or finite number; temperatures use Celsius.'],
  'devices.refresh': ['target', 'boolean | nil, error', 'Request a device refresh.'],
  'scenes.get': ['group', 'group | nil, error', 'Read an immutable gateway scene selection and group-off state; this is not a physical state match.'],
  'scenes.activate': ['group, scene', 'boolean | nil, error', 'Activate a scene using names or string IDs.'],
  'scenes.off': ['group', 'boolean | nil, error', 'Turn off a scene group.'],
  'notify.send': ['title, body, options', 'boolean | nil, error', 'Send a notification; optional options: device_id, tag, recipients (one client ID/label or a list). Explicit recipients ignore presence; notification switches still apply.'],
  log: ['level, message, fields', 'nil', 'Log debug/info/warn/error; optional JSON-compatible fields.'],
  dump: ['value, label', 'nil', 'Log a structured value with optional label.'],
}
export const eventNames = ['device.added', 'device.removed', 'device.value', 'device.changed', 'remote.action', 'device.online', 'device.offline', 'timer.fired', 'automation.error']
export const filterKeys = ['type', 'device', 'device_id', 'source', 'field', 'value', 'action', 'endpoint', 'position', 'press_count', 'value_below', 'value_at_most', 'value_above', 'value_at_least']
export const snapshotFields = ['id', 'name', 'source', 'kind', 'online', 'last_seen', 'temperature_celsius', 'humidity_percent', 'contact_open', 'water_detected', 'occupied', 'light_on', 'actor_on', 'power_watts', 'air_quality', 'co2_ppm', 'pm25_micrograms_m3', 'brightness_percent', 'battery_percent', 'wifi_signal_percent', 'link_quality', 'thermostat']
export const remoteActions = ['on', 'off', 'toggle', 'move-to-level', 'move', 'step', 'stop', 'arrow_left', 'arrow_right', 'arrow_left_hold', 'arrow_right_hold', 'arrow_left_release', 'arrow_right_release', 'matter_press', 'matter_hold', 'matter_release', 'matter_multi_press']
const sources = ['zigbee', 'shelly', 'fritz_box', 'matter']
const keywords = ['and','break','do','else','elseif','end','false','for','function','goto','if','in','local','nil','not','or','repeat','return','then','true','until','while']
const namespaces = ['triggers', 'events', 'automation', 'devices', 'scenes', 'notify', 'persistent', 'shared', 'math', 'string', 'table']
export const argumentTypes: Record<string, string> = {
  id: 'string (stable trigger ID)', filter: 'table (optional for devices.list)', callback: 'function', handle: 'handle', timeout: 'number? (seconds)', seconds: 'number (0 < seconds <= 63072000)', payload: 'JSON value?', expression: 'string (five cron fields)', target: 'string (ID or unique name)', field: 'string (thermostat field name)', on: 'boolean', state: 'table {on?: boolean, brightness?: integer 0–100}', group: 'string (ID or unique name)', scene: 'string (ID or unique name)', title: 'string', body: 'string', options: 'table? {device_id?: string, tag?: string}', level: 'string (debug/info/warn/error)', message: 'string', fields: 'JSON value?', value: 'any', label: 'string?',
}
const globals = ['assert', 'error', 'ipairs', 'next', 'pairs', 'pcall', 'select', 'tonumber', 'tostring', 'type', 'xpcall', 'rawequal', '_VERSION', '_G']
const standard: Record<string, string[]> = {
  math: ['abs','acos','asin','atan','ceil','cos','deg','exp','floor','fmod','huge','log','max','maxinteger','min','mininteger','modf','pi','rad','random','randomseed','sin','sqrt','tan','tointeger','type','ult'],
  string: ['byte','char','dump','find','format','gmatch','gsub','len','lower','match','pack','packsize','rep','reverse','sub','unpack','upper'],
  table: ['concat','insert','move','pack','remove','sort','unpack'],
}
const fieldCapabilities: Record<string, keyof Device> = {
  temperature_celsius:'has_temperature', humidity_percent:'has_humidity', contact_open:'has_contact', water_detected:'has_water_leak', occupied:'has_occupancy', light_on:'has_light', actor_on:'has_actor', power_watts:'has_power', air_quality:'has_air_quality', co2_ppm:'has_co2', pm25_micrograms_m3:'has_pm25', brightness_percent:'has_brightness', battery_percent:'has_battery', wifi_signal_percent:'has_wifi_signal', thermostat:'has_thermostat',
}
const entry = (value: string, meta = 'Lua', score = 1000): Completion => ({ caption: value, value, meta, score })
const fieldType = (name: string) => ['id','name','source','kind','type','field','action','reason','message','operation','timer_id','chain_id','device_id','air_quality'].includes(name) ? 'string' : name === 'recipients' ? 'client ID or unique label, or a list of IDs/labels' : name === 'tag' ? 'string (user-defined)' : ['online','contact_open','water_detected','occupied','light_on','actor_on'].includes(name) ? 'boolean' : name === 'thermostat' ? 'immutable thermostat snapshot' : name === 'device' ? 'immutable device snapshot' : ['value','old','payload'].includes(name) ? 'event-dependent value' : 'number'
const documentation = (name: string, spec: Spec) => `${name}(${spec[0].split(', ').filter(Boolean).map(arg => `${arg}: ${argumentTypes[arg]}`).join(', ')}): ${spec[1]}\n${spec[2]}`
const quote = (value: string, delimiter = '"') => delimiter + value.replace(/\\/g, '\\\\').replace(/\n/g, '\\n').replace(/\r/g, '\\r').replace(new RegExp(delimiter, 'g'), '\\' + delimiter) + delimiter

// A tolerant lexer keeps strings/comments opaque and tolerates incomplete documents.
export function tokens(source: string): string[] {
  return source.match(/--\[(=*)\[[\s\S]*?\]\1\]|--[^\n]*|\[(=*)\[[\s\S]*?\]\2\]|"(?:\\.|[^"\\])*"?|'(?:\\.|[^'\\])*'?|[A-Za-z_][\w]*|\d+(?:\.\d+)?|[^\s]/g)?.filter(token => !token.startsWith('--')) ?? []
}
function stackOf(ts: string[]) {
  const stack: { kind: string; at: number; call: string; argument: number }[] = []
  ts.forEach((token, at) => {
    if (['(', '{', '['].includes(token)) stack.push({ kind: token, at, call: token === '(' ? ts.slice(Math.max(0, at - 3), at).join('').match(/(?:\w+\.)?\w+$/)?.[0] ?? '' : '', argument: 0 })
    else if ([')', '}', ']'].includes(token)) stack.pop()
    else if (token === ',' && stack.length) stack[stack.length - 1].argument++
  })
  return stack
}
export function completions(source: string, offset: number, catalog: Catalog): Completion[] {
  const before = source.slice(0, offset)
  // Mask comments and long strings including unfinished ones, without interpreting their contents.
  const opaque = /--\[(=*)\[[\s\S]*?(?:\]\1\]|$)|--[^\n]*(?:\n|$)|\[(=*)\[[\s\S]*?(?:\]\2\]|$)|"(?:\\.|[^"\\])*(?:"|$)|'(?:\\.|[^'\\])*(?:'|$)/g
  let inString = '', stringStart = -1
  for (const match of before.matchAll(opaque)) {
    if (match.index! + match[0].length !== before.length) continue
    const text = match[0]
    if (text.startsWith('--') && !text.endsWith('\n') || text.startsWith('[')) return []
    if ((text[0] === '"' || text[0] === "'") && (text.length === 1 || text[text.length - 1] !== text[0] || /\\(?:\\\\)*["']$/.test(text))) { inString = text[0]; stringStart = match.index! }
  }
  const prefix = inString ? before.slice(stringStart + 1).replace(/\\([\\"'])/g, '$1') : before.match(/[\w]*$/)?.[0] ?? ''
  const context = inString ? before.slice(0, stringStart) : before.slice(0, before.length - prefix.length)
  const ts = tokens(context), stack = stackOf(ts)
  const call = [...stack].reverse().find(frame => frame.kind === '(' && frame.call)
  const table = [...stack].reverse().find(frame => frame.kind === '{' && (!call || frame.at > call.at))
  const key = ts.slice(-1)[0] === '=' ? ts.slice(-2)[0] : undefined
  const tableText = table ? ts.slice(table.at).join(' ') : ''
  const selectedId = tableText.match(/\bdevice(?:_id)?\s*=\s*(["'])(.*?)\1/)?.[2]
  const selectedDevice = catalog.devices.find(d => [d.ieee_address, d.name].includes(selectedId ?? ''))
  const eventType = tableText.match(/\btype\s*=\s*(["'])(.*?)\1/)?.[2]
  const eventCall = call && ['events.on', 'events.wait_for'].includes(call.call)
  let result: Completion[] = []
  const values = (items: string[], meta: string) => items.map(value => entry(inString ? value : quote(value), meta))
  const deviceValues = (idsOnly = false) => {
    const list: Completion[] = []
    for (const device of catalog.devices) {
      // Automation uses ieee_address verbatim for every protocol, unlike dashboard IDs.
      const id = device.ieee_address
      if (!id) continue
      const capability = { 'devices.actor': device.has_actor, 'devices.light': device.has_light, 'devices.thermostat': device.has_thermostat }
      const compatible = !call || !(call.call in capability) || capability[call.call as keyof typeof capability]
      const meta = `${device.kind} · ${device.source}${compatible ? '' : ' · unsupported capability'}`
      const add = (value: string, score: number) => list.push({ ...entry(inString ? value : quote(value), meta, score), caption: `${device.name || id} — ${value}` })
      add(id, compatible ? 2000 : 500)
      if (!idsOnly && device.name && catalog.devices.filter(d => d.name.toLowerCase() === device.name.toLowerCase()).length === 1) add(device.name, compatible ? 1500 : 400)
    }
    return list
  }
  if ((key === 'device' || key === 'device_id') || call?.call.startsWith('devices.') && call.call !== 'devices.list' && call.argument === 0 && !table) result = deviceValues(key === 'device_id')
  else if (call?.call === 'devices.thermostat' && call.argument === 1 && !table) {
    const target = context.slice(context.lastIndexOf('(') + 1).match(/^\s*(["'])(.*?)\1/)?.[2]
    const device = catalog.devices.find(d => d.ieee_address === target || d.name.toLowerCase() === target?.toLowerCase())
    const thermostat = device?.thermostat
    const fields = thermostat ? Object.keys(thermostat.fields).filter(field => thermostat.supported[field] && thermostat.fields[field].writable) : []
    if (thermostat?.schedule_supported) fields.push(...Array.from({ length: 7 }, (_, day) => `schedule_${day}`))
    if (thermostat?.external_input_supported) fields.push('temperature_source', 'stale_minutes')
    result = values(fields, 'thermostat field')
  }
  else if (key === 'type' && eventCall) result = values(eventNames, 'event type')
  else if (key === 'source') result = values(sources, 'device source')
  else if (key === 'kind') result = values([...new Set(catalog.devices.map(d => d.kind))], 'device kind')
  else if (key === 'name' && call?.call === 'devices.list') result = values([...new Set(catalog.devices.map(d => d.name))], 'device name')
  else if (key === 'field' && eventCall) result = values(snapshotFields.filter(field => !selectedDevice || !fieldCapabilities[field] || selectedDevice[fieldCapabilities[field]]), 'snapshot field')
  else if (key === 'action' && eventCall) result = values(remoteActions.filter(action => !selectedDevice || (selectedDevice.source === 'matter') === action.startsWith('matter_')), 'remote action')
  else if (call?.call === 'devices.actor' && call.argument === 1 || ['on', 'online'].includes(key ?? '') || key === 'value' && /(?:contact_open|water_detected|occupied|light_on|actor_on|online)/.test(tableText)) result = ['true','false'].map(v => entry(v, 'boolean'))
  else if (key === 'endpoint' && selectedDevice) result = [...new Set(selectedDevice.endpoint_descriptors.map(e => e.endpoint))].map(n => entry(String(n), 'endpoint'))
  else if (call?.call === 'log' && call.argument === 0) result = values(['debug','info','warn','error'], 'log severity')
  else if (call?.call.startsWith('scenes.') && !table) {
    const firstArg = context.slice(context.lastIndexOf('(') + 1).match(/^\s*(["'])(.*?)\1/)?.[2]
    const group = catalog.groups.find(g => String(g.id) === firstArg || g.name === firstArg)
    const items = call.argument === 0 ? catalog.groups : group?.scenes ?? []
    result = items.flatMap(item => values([String(item.id), ...(!items.some(other => other !== item && other.name.toLowerCase() === item.name.toLowerCase()) ? [item.name] : [])], call.argument === 0 ? 'scene group' : 'scene'))
  } else if (!inString && table && ['{', ',', ';'].includes(ts.slice(-1)[0])) {
    let keys: string[] = []
    if (eventCall && call.argument === 0) keys = filterKeys.filter(k => !eventType || eventType === 'remote.action' || !['action','endpoint','position','press_count'].includes(k))
    else if (call?.call === 'devices.light' && call.argument === 1) keys = ['on','brightness']
    else if (call?.call === 'devices.list') keys = ['source','kind','online','name']
    else if (call?.call === 'notify.send' && call.argument === 2) keys = ['device_id','tag','recipients']
    result = keys.filter(k => !new RegExp(`\\b${k}\\s*=`).test(tableText)).map(k => ({ ...entry(k + ' = ', 'argument field'), docText: k.startsWith('value_') ? `${k}: finite number — compare the event value` : k === 'brightness' ? 'brightness: integer 0–100' : ['endpoint','position','press_count'].includes(k) ? `${k}: integer` : `${k}: ${fieldType(k)}` }))
  } else if (!inString) {
    const member = context.match(/([\w.]+)\.$/)?.[1]
    if (member) {
      result = Object.entries(gatewayApi).filter(([name]) => name.startsWith(member + '.')).map(([name, spec]) => ({ ...entry(name.slice(member.length + 1), 'gateway API'), caption: `${name}(${spec[0]})`, docText: documentation(name, spec), snippet: `${name.slice(member.length + 1)}(${spec[0].split(', ').filter(Boolean).map((arg, i) => '${' + (i + 1) + ':' + arg + '}').join(', ')})` }))
      result.push(...(standard[member] ?? []).map(name => entry(name, 'Lua standard library')))
      // Infer directly registered event callbacks, using their actual parameter name.
      const callbacks = [...context.matchAll(/events\.on\s*\(\s*\{([^}]*?)\}\s*,\s*function\s*\(\s*(\w+)\s*\)/g)]
      const cb = callbacks.reverse().find(candidate => {
        let depth = 1
        for (const token of tokens(context.slice(candidate.index! + candidate[0].length))) {
          if (['function','if','do','repeat'].includes(token)) depth++
          if (token === 'end' || token === 'until') depth--
          if (depth === 0) return false
        }
        return true
      })
      if (cb && (member === cb[2] || member === cb[2] + '.device')) {
        const type = cb[1].match(/type\s*=\s*["']([^"']+)/)?.[1]
        const extra = !type ? ['device','device_id','source','field','value','old','action','endpoint','position','press_count','reason','timer_id','payload','message','operation'] : type === 'remote.action' ? ['device','device_id','source','action','endpoint','position','press_count'] : type?.startsWith('device.') ? ['device','device_id','source', ...(type === 'device.removed' ? ['reason'] : type === 'device.changed' || type === 'device.value' ? ['field','value', ...(type === 'device.changed' ? ['old'] : [])] : [])] : type === 'timer.fired' ? ['source','timer_id','payload'] : ['source','message','operation']
        result = (member.endsWith('.device') ? (!type || type.startsWith('device.') || type === 'remote.action' ? snapshotFields : []) : ['type','at','chain_id','chain_depth', ...extra]).map(field => ({ ...entry(field, fieldType(field)), docText: `${field}: ${fieldType(field)} — immutable event data` }))
      }
    } else {
      result = [...namespaces, ...globals, ...keywords].map(v => entry(v, 'sandbox'))
      result.push(...Object.entries(gatewayApi).filter(([name]) => !name.includes('.')).map(([name, spec]) => ({ ...entry(name, 'gateway API'), docText: documentation(name, spec) })))
    }
  }
  return result.map(item => inString ? { ...item, stringStart: stringStart + 1, delimiter: inString } : item).filter(item => (item.value.startsWith('"') || item.value.startsWith("'") ? item.value.slice(1) : item.value).toLowerCase().startsWith(prefix.toLowerCase()) || item.caption.toLowerCase().startsWith(prefix.toLowerCase()))
}

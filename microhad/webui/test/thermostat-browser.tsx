import { createRoot } from 'react-dom/client'
import { ThermostatPanel } from '../src/ThermostatPanel'
import { scheduleError } from '../src/ThermostatDialog'
import type { Device, ThermostatSchedule, ThermostatState } from '../src/api'

let t: ThermostatState = {
  fields: { occupied_heating_setpoint: { min: 4, max: 35, step: .5, writable: true }, occupied_cooling_setpoint: { min: 10, max: 40, step: .5, writable: true }, child_lock: { min: 0, max: 1, step: 1, writable: true } },
  modes: [0, 1, 4], schedule_max_transitions: 6, schedule_midnight_start: true,
  schedule_supported: true, external_input_supported: true,
  schedule_details: [null, { mode: 1, transitions: [{ time: 0, heat: 20 }] }, null, null, null, null, null],
  pending: [], values: { occupied_heating_setpoint: 20, system_mode: 1, child_lock: 0 },
  supported: { occupied_heating_setpoint: true, system_mode: true, child_lock: true, temperature_sensor_select: true },
  schedules: [], source_id: '', stale_minutes: 30, source_observed_at: 0, status: '', error: '',
}
const requests: { field: string; value: string }[] = []
let fail = ''
let activeRequests = 0, maxRequests = 0
window.fetch = async (_url, options) => {
  const fields = new URLSearchParams(String(options?.body))
  const field = fields.get('field')!, value = fields.get('value')!
  requests.push({ field, value }); activeRequests++; maxRequests = Math.max(activeRequests, maxRequests)
  await new Promise(resolve => setTimeout(resolve, 30)); activeRequests--
  return new Response(JSON.stringify(field === fail ? { error: 'Synthetic failure' } : { ok: true }), { status: field === fail ? 400 : 200 })
}
const root = createRoot(document.getElementById('root')!)
let available = true
function render() { root.render(<ThermostatPanel device={{ id: 'test-valve', name: 'Living room', thermostat: t, controller_available: available } as Device} sources={[{ id: 'sensor', name: 'Room sensor', observed_at: 0 }]} />) }
const pause = () => new Promise(resolve => setTimeout(resolve, 50))
async function until(check: () => boolean) { for (let i = 0; i < 100; i++) { if (check()) return; await pause() }; throw new Error('Timed out: ' + document.body.textContent) }
function assert(value: unknown, message: string) { if (!value) throw new Error(message) }
function button(text: string) { const result = [...document.querySelectorAll<HTMLButtonElement>('button')].find(b => b.textContent === text); if (!result) throw new Error('Missing button: ' + text); return result }
function field(text: string) { const label = [...document.querySelectorAll('dialog label')].find(l => [...l.childNodes].filter(n => n.nodeType === Node.TEXT_NODE).map(n => n.textContent).join('') === text); if (!label) throw new Error('Missing field: ' + text); return label.querySelector<HTMLInputElement | HTMLSelectElement>('input,select')! }
async function set(element: HTMLInputElement | HTMLSelectElement, value: string) {
  Object.getOwnPropertyDescriptor(element instanceof HTMLInputElement ? HTMLInputElement.prototype : HTMLSelectElement.prototype, 'value')!.set!.call(element, value)
  element.dispatchEvent(new Event(element instanceof HTMLInputElement ? 'input' : 'change', { bubbles: true })); await pause()
}
function day(name: string) { return [...document.querySelectorAll<HTMLButtonElement>('.thermostat-week-day')].find(b => b.querySelector('strong')?.textContent === name)! }
async function open() { button('Thermostat settings').focus(); button('Thermostat settings').click(); await until(() => !!document.querySelector('dialog[open]')) }
async function schedule() { button('Weekly schedule').click(); await pause() }
async function save() { button('Save changes').click(); await until(() => button('Save changes').textContent === 'Save changes' && !document.body.textContent!.includes('Sending requests…')) }
function checkValidation() {
  const invalid = (s: ThermostatSchedule) => !!scheduleError(s, t)
  assert(invalid({ mode: 1, transitions: [] }), 'Empty schedule accepted')
  assert(invalid({ mode: 1, transitions: [{ time: 60, heat: 20 }] }), 'Midnight rule ignored')
  assert(invalid({ mode: 1, transitions: [{ time: 0, heat: 20 }, { time: 0, heat: 21 }] }), 'Duplicate time accepted')
  assert(invalid({ mode: 1, transitions: [{ time: 0, heat: NaN }] }), 'Empty temperature accepted')
  assert(invalid({ mode: 1, transitions: [{ time: 0, heat: 20.1 }] }), 'Temperature step ignored')
  assert(invalid({ mode: 2, transitions: [{ time: 0, cool: 25 }] }), 'Unsupported cooling accepted')
  assert(invalid({ mode: 1, transitions: Array.from({ length: 7 }, (_, i) => ({ time: i * 60, heat: 20 })) }), 'Transition limit ignored')
}
async function run() {
  checkValidation(); render(); await until(() => !!document.querySelector('.thermostat-panel')); await open()
  assert(requests.length === 0, 'Opening caused device reads')
  button('Settings').dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowRight', bubbles: true })); await pause()
  assert(document.activeElement === button('Weekly schedule') && button('Weekly schedule').getAttribute('aria-selected') === 'true', 'Arrow keys did not select and focus schedule tab')
  button('Weekly schedule').dispatchEvent(new KeyboardEvent('keydown', { key: 'Home', bubbles: true })); await pause()
  assert(document.querySelector('dialog')!.contains(document.activeElement), 'Opening did not focus dialog')
  assert(document.querySelector('dialog')!.getBoundingClientRect().width <= window.innerWidth, 'Dialog overflows mobile viewport')
  await set(field('Child lock'), '1')
  t = { ...t, values: { ...t.values, child_lock: 0 } }; render(); await pause()
  assert(field('Child lock').value === '1', 'Polling overwrote setting draft')
  button('Close').click(); await pause(); assert(document.body.textContent!.includes('Discard unsaved changes?'), 'Dirty close bypassed confirmation')
  button('Keep editing').click(); await pause()
  await schedule()
  assert([...document.querySelectorAll('.thermostat-week-day strong')].map(x => x.textContent).join(',') === 'Monday,Tuesday,Wednesday,Thursday,Friday,Saturday,Sunday', 'Week order incorrect')
  await set(field('Heat (°C)'), '21')
  t = { ...t, schedule_details: [...t.schedule_details] }; render(); await pause()
  assert(field('Heat (°C)').value === '21', 'Polling overwrote schedule draft')
  button('Copy to…').click(); await pause()
  ;[...document.querySelectorAll<HTMLInputElement>('.thermostat-copy-days input')].filter(input => ['Tuesday', 'Wednesday'].includes(input.parentElement!.textContent!)).forEach(input => input.click())
  await pause(); button('Stage copy').click(); await pause()
  assert(requests.length === 0, 'Staged copy made a request')
  assert(day('Tuesday').textContent!.includes('Unsaved changes'), 'Copy did not stage Tuesday')
  fail = 'schedule_3'; await save()
  await until(() => requests.length === 4 && button('Save changes').disabled === false)
  assert(maxRequests === 1, 'Save requests were concurrent')
  assert(requests.map(r => r.field).join(',') === 'child_lock,schedule_1,schedule_2,schedule_3', 'Save fields or day numbering incorrect')
  assert(requests.filter(r => r.field.startsWith('schedule_')).every(r => JSON.parse(r.value).transitions[0].heat === 21), 'Copied payload wrong')
  assert(day('Wednesday').textContent!.includes('Needs attention'), 'Partial failure not marked')
  assert(field('Heat (°C)').value === '21' && field('Heat (°C)').matches(':disabled'), 'Queued draft reverted to old report')
  fail = ''; await save(); await until(() => requests.length === 5 && button('Save changes').disabled)
  assert(requests[4].field === 'schedule_3', 'Retry resent successful days')
  t = { ...t, values: { ...t.values, child_lock: 1 }, schedule_details: t.schedule_details.map((entry, index) => [1, 2, 3].includes(index) ? { mode: 1, transitions: [{ time: 0, heat: 21 }] } : entry) }; render()
  await until(() => !field('Heat (°C)').matches(':disabled'))
  assert(day('Monday').textContent!.includes('Confirmed'), 'Device confirmation missing')
  day('Thursday').click(); await pause(); button('Read from device').click(); await until(() => requests.length === 6 && !document.body.textContent!.includes('Sending requests…'))
  assert(requests[5].field === 'schedule_4' && requests[5].value === '__read__', 'Read payload wrong')
  t = { ...t, pending: ['schedule_4'] }; render(); await pause()
  t = { ...t, pending: [], schedule_details: t.schedule_details.map((entry, index) => index === 4 ? { mode: 1, transitions: [{ time: 0, heat: 19 }] } : entry) }; render(); await pause()
  assert(document.body.textContent!.includes('Read complete'), 'Read completion missing')
  day('Monday').click(); await pause(); button('Add transition').click(); await pause()
  const times = document.querySelectorAll<HTMLInputElement>('.thermostat-transition input[type=time]')
  await set(times[1], '00:00'); await save()
  assert(requests.length === 6 && document.body.textContent!.includes('unique start times'), 'Invalid schedule was sent')
  await set(times[1], '06:00')
  available = false; render(); await pause(); assert(button('Save changes').disabled, 'Unavailable controller can save')
  available = true; render(); await pause()
  // Replacing a destination draft requires a separate explicit click.
  day('Tuesday').click(); await pause(); await set(field('Heat (°C)'), '22')
  day('Monday').click(); await pause(); button('Copy to…').click(); await pause()
  ;[...document.querySelectorAll<HTMLInputElement>('.thermostat-copy-days input')].find(input => input.parentElement!.textContent === 'Tuesday')!.click()
  await pause(); button('Stage copy').click(); await pause(); assert(!!button('Replace drafts'), 'Draft replacement lacked confirmation')
  button('Replace drafts').click(); await pause(); day('Tuesday').click(); await pause()
  assert(field('Heat (°C)').value === '21', 'Confirmed copy failed')
  document.querySelector('dialog')!.dispatchEvent(new Event('cancel', { cancelable: true })); await pause()
  assert(document.body.textContent!.includes('Discard unsaved changes?'), 'Escape discarded edits')
  button('Discard changes').click(); await until(() => !document.querySelector('dialog'))
  assert(document.activeElement === button('Thermostat settings'), 'Focus was not restored')
  // Cooling devices expose only their supported modes and honor the temperature gap.
  t = { ...t, supported: { occupied_cooling_setpoint: true }, modes: [0, 1, 3], external_input_supported: false, schedule_midnight_start: false, schedule_details: [null, null, null, null, null, null, null] }
  render(); await open(); await schedule()
  assert(field('Schedule type').value === '2', 'Cooling-only device default incorrect')
  button('Add transition').click(); await pause(); assert(!!field('Cool (°C)'), 'Cooling input absent')
  assert(!document.body.textContent!.includes('Heat (°C)'), 'Unsupported heating shown')
  t = { ...t, supported: { occupied_heating_setpoint: true, occupied_cooling_setpoint: true } }; render(); await pause()
  await set(field('Schedule type'), '3'); await set(field('Cool (°C)'), '21'); await save()
  assert(document.body.textContent!.includes('minimum temperature gap'), 'Heating/cooling gap not validated')
  assert(document.querySelector('dialog')!.scrollWidth <= document.querySelector('dialog')!.clientWidth, 'Schedule overflows mobile dialog')
  button('Close').click(); await pause(); button('Discard changes').click(); await until(() => !document.querySelector('dialog'))
  t = { ...t, error: '', supported: { child_lock: true }, values: { child_lock: 0 }, pending: [] }; render(); await open()
  await set(field('Child lock'), '1')
  const before = requests.length
  button('Save changes').click(); button('Save changes').click()
  await until(() => requests.length === before + 1 && button('Save changes').disabled)
  assert(requests.length === before + 1, 'Duplicate save sent twice')
  t = { ...t, pending: ['child_lock'] }; render(); await pause()
  t = { ...t, pending: [], error: 'Device rejected change' }; render(); await pause()
  assert(field('Child lock').value === '1' && !field('Child lock').matches(':disabled'), 'Device failure did not restore editable draft')
  assert(document.body.textContent!.includes('Device rejected change'), 'Device failure not displayed')
  t = { ...t, error: '' }; render(); await pause(); await save()
  await until(() => requests.length === before + 2 && button('Save changes').disabled)
  t = { ...t, values: { child_lock: 1 } }; render(); await pause()
  assert(document.body.textContent!.includes('Confirmed'), 'Retried setting was not confirmed')
  document.getElementById('result')!.dataset.result = 'passed'
  document.getElementById('result')!.textContent = 'PASS thermostat dialog drafts, focus, validation, copy, sequential save, partial failure, confirmation, capabilities and mobile layout'
}
void run().catch(error => { document.getElementById('result')!.textContent = String(error) })

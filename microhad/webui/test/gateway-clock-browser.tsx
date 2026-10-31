import { createRoot } from 'react-dom/client'
import { GatewaySettingsDialog } from '../src/GatewaySettingsDialog'
import { DeviceSettings } from '../src/devices'
import type { Device } from '../src/api'

const root = createRoot(document.getElementById('root')!)
const node = '0000000000000002'
let settings = { time_zone: 'Europe/Berlin', time_zones: ['UTC', 'Europe/Berlin', 'Asia/Kolkata'],
  broadcast_passive_ack_quorum: 8, matter_country_code: 'DE', matter_regulatory_location: 'indoor',
  applied_value: null, application_state: 'unsupported', application_error: '' }
let saved: URLSearchParams | undefined
let clock: Record<string, unknown> | undefined
const response = (value: unknown) => new Response(JSON.stringify(value), { status: 200 })
window.fetch = async (input, init) => {
  const url = String(input)
  if (url === '/api/auth/status') return response({ role: 2, passwords: [false, false, false] })
  if (url === '/api/settings') {
    if (init?.method === 'POST') {
      saved = new URLSearchParams(String(init.body))
      settings = { ...settings, time_zone: saved.get('time_zone')! }
    }
    return response(settings)
  }
  if (url === '/api/matter/devices/command') {
    clock = JSON.parse(String(init?.body))
    return response({ schema: 1, ok: true, job: 1, state: 'queued' })
  }
  if (url === '/api/matter/jobs/1') return response({ job: 1, state: 'complete', response: { schema: 1, ok: true,
    result: { state: 'complete', job: '0000000000000001', result: { node, endpoint: 0, cluster: 56, attribute: 0, value: { type: 'bool', value: true } } } } })
  throw new Error(`Unexpected request ${url}`)
}
function assert(value: unknown, message: string): asserts value { if (!value) throw new Error(message) }
const pause = () => new Promise(resolve => setTimeout(resolve, 20))
async function until(check: () => boolean) {
  for (let i = 0; i < 100; i++) { if (check()) return; await pause() }
  throw new Error('Timed out: ' + document.body.textContent)
}
function button(text: string) {
  const found = [...document.querySelectorAll<HTMLButtonElement>('button')].find(b => b.textContent === text)
  assert(found, `Missing button ${text}`); return found
}
async function run() {
  root.render(<GatewaySettingsDialog onClose={() => {}} />)
  await until(() => [...document.querySelectorAll('option')].some(o => o.value === 'Asia/Kolkata'))
  const select = [...document.querySelectorAll<HTMLSelectElement>('select')].find(s => s.value === 'Europe/Berlin')!
  assert(select, 'Saved gateway timezone was not loaded')
  Object.getOwnPropertyDescriptor(HTMLSelectElement.prototype, 'value')!.set!.call(select, 'Asia/Kolkata')
  select.dispatchEvent(new Event('change', { bubbles: true })); await pause()
  button('Save settings').click()
  await until(() => !!saved)
  assert(saved!.get('time_zone') === 'Asia/Kolkata', 'Gateway timezone was not saved')
  const device = { name: 'ALPSTUGA', source: 'matter', matter_node: node, online: true,
    endpoint_descriptors: [{ endpoint: 0, input_clusters: [56], output_clusters: [] }],
    value_alerts: {} } as unknown as Device
  root.render(<DeviceSettings device={device} onClose={() => {}} onSaved={() => {}} />)
  await until(() => document.body.textContent!.includes('Synchronize clock now'))
  assert(!document.body.textContent!.includes('Time zone'), 'A conflicting per-device timezone selector remains')
  button('Synchronize clock now').click()
  await until(() => document.body.textContent!.includes('Clock synchronized.'))
  assert(clock?.command === 'sync_time' && clock.node === node && clock.endpoint === 0, 'Wrong clock request')
  assert(!('time_zone' in clock!) && !('offset_seconds' in clock!), 'Browser timezone overrides gateway settings')
  document.getElementById('result')!.textContent = 'PASS gateway clock timezone settings and Matter sync'
  document.body.dataset.result = 'passed'
  root.unmount()
}
run().catch(error => { document.getElementById('result')!.textContent = String(error); document.body.dataset.result = 'failed' })

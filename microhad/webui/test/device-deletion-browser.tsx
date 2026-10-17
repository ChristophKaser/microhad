import { useState } from 'react'
import { createRoot } from 'react-dom/client'
import type { Device } from '../src/api'
import { DeviceSettings } from '../src/devices'
import { DeleteDeviceDialog, IncompleteDevices, incompleteDevices } from '../src/DeviceDeletion'

const zigbee = { id: '0x00124b0000000002', ieee_address: '0x00124b0000000002', source: 'zigbee', name: 'Unfinished sensor',
  endpoint_descriptors: [], setup_state: 'incomplete', value_alerts: {} } as unknown as Device
const paired = { ...zigbee, id: '0x00124b0000000003', name: 'Paired sensor', setup_state: 'complete', online: false } as Device
const matter = { ...zigbee, id: 'matter:0000000000000001:0000000000000002', name: 'Interrupted Matter', source: 'matter',
  matter_node: '0000000000000002', commissioning_state: 'recovery_required' } as Device
const external = { ...zigbee, id: 'shelly:123', name: 'External', source: 'shelly' } as Device
let scenario = 'zigbee'
let posts: string[] = []
let polls = 0
let removed = 0
let closed = 0
window.fetch = async (url, options) => {
  const path = String(url)
  const response = (value: unknown, status = 200) => new Response(JSON.stringify(value), { status, headers: { 'Content-Type': 'application/json' } })
  if (options?.method === 'POST') {
    const mode = new URLSearchParams(String(options.body)).get('mode')!
    posts.push(mode)
    if (scenario === 'busy') return response({ ok: false, error: 'busy' }, 400)
    if (scenario === 'storage') return response({ ok: false, error: 'storage' }, 400)
    return response({ ok: true, state: 'pending', ...(scenario.startsWith('matter') ? { job: 7 } : {}) })
  }
  ++polls
  if (scenario === 'matter-busy') return response({ job: 7, state: 'complete', response: { ok: false, error: 'busy' } })
  if (scenario === 'matter') return response(polls === 1 ? { job: 7, state: 'running' } :
    { job: 7, state: 'complete', response: { ok: true, result: { state: 'complete' } } })
  if (scenario === 'timeout' && posts.at(-1) !== 'local') return response({ state: 'failed', error: 'timeout' })
  if (!path.endsWith('/delete')) throw new Error(`Unexpected fetch ${path}`)
  return response(polls === 1 ? { state: 'pending' } : { state: 'succeeded' })
}
function Host({ settings = false, device = zigbee }: { settings?: boolean; device?: Device }) {
  const [selected, select] = useState<Device | null>(settings ? device : null)
  const [list, setList] = useState([device, paired])
  const done = () => { ++removed; select(null); setList(items => items.filter(item => item.id !== device.id)) }
  return <><IncompleteDevices devices={list} onDelete={select} />
    {selected && (settings ? <DeviceSettings device={selected} onClose={() => { ++closed; select(null) }} onSaved={() => {}} onDeleted={done} /> :
      <DeleteDeviceDialog device={selected} onClose={() => { ++closed; select(null) }} onDeleted={done} />)}</>
}
const root = createRoot(document.getElementById('root')!)
const pause = () => new Promise(resolve => setTimeout(resolve, 10))
async function until(check: () => boolean) { for (let n = 0; n < 250; ++n) { if (check()) return; await pause() } throw new Error('UI wait timed out: ' + document.body.textContent) }
function check(value: unknown, message: string) { if (!value) throw new Error(message) }
function button(label: string): HTMLButtonElement {
  const found = [...document.querySelectorAll('button')].find(button => button.textContent?.trim() === label)
  check(found, `Missing button: ${label}`); return found!
}
let key = 0
async function render(next: string, settings = false, device = zigbee) {
  scenario = next; posts = []; polls = 0
  root.render(<Host key={++key} settings={settings} device={device} />)
  await until(() => settings ? document.body.textContent!.includes('Device settings') : document.body.textContent!.includes('Incomplete setup'))
  await pause()
}
async function run() {
  check(incompleteDevices([zigbee, paired, matter, { ...matter, id: matter.id + ':0001' }, external]).length === 2, 'Incomplete list must exclude paired/offline/external devices and deduplicate Matter nodes')
  check(incompleteDevices([{ ...zigbee, setup_state: undefined, endpoint_descriptors: [{ endpoint: 1 }] } as Device]).length === 0, 'Legacy discovered Zigbee device must stay complete')
  await render('zigbee')
  button('Delete').click(); await until(() => document.body.textContent!.includes('Unpair and delete'))
  button('Cancel').click(); await until(() => !document.querySelector('[role="dialog"]'))
  check(posts.length === 0 && closed === 1, 'Cancel sent a deletion request')
  button('Delete').click(); await until(() => !!document.querySelector('[role="dialog"]'))
  button('Unpair and delete').click(); await pause()
  check(button('Cancel').disabled, 'Controls were not disabled while deleting')
  await until(() => removed === 1)
  check(posts.join(',') === 'unpair' && !document.body.textContent!.includes('Unfinished sensor'), 'Successful deletion did not refresh the list')
  await render('timeout')
  button('Delete').click(); await until(() => !!document.querySelector('[role="dialog"]'))
  button('Unpair and delete').click(); await until(() => document.body.textContent!.includes('Delete locally'))
  check(document.body.textContent!.includes('factory reset'), 'Local fallback consequences missing')
  button('Delete locally').click(); await until(() => removed === 2)
  check(posts.join(',') === 'unpair,local', 'Local deletion was not explicitly selected')
  await render('busy')
  button('Delete').click(); await until(() => !!document.querySelector('[role="dialog"]'))
  button('Unpair and delete').click(); await until(() => document.body.textContent!.includes('Cancel commissioning'))
  check(!document.body.textContent!.includes('Delete locally'), 'Busy commissioning must not allow local fallback')
  await render('storage')
  button('Delete').click(); await until(() => !!document.querySelector('[role="dialog"]'))
  button('Unpair and delete').click(); await until(() => document.body.textContent!.includes('Check storage'))
  check(!document.body.textContent!.includes('Delete locally'), 'Storage failure must remain retryable without fallback')
  await render('matter-busy', false, matter)
  button('Delete').click(); await until(() => !!document.querySelector('[role="dialog"]'))
  button('Unpair and delete').click(); await until(() => document.body.textContent!.includes('Cancel commissioning'))
  check(!document.body.textContent!.includes('Delete locally'), 'Asynchronous busy reply allowed local fallback')
  await render('matter', true, matter)
  button('Delete device').click(); await until(() => document.body.textContent!.includes('All endpoints'))
  button('Unpair and delete').click(); await until(() => removed === 3)
  check(polls >= 2 && posts.length === 1, 'Matter job was not polled to completion')
  await render('zigbee', true, external)
  check(!document.body.textContent!.includes('Delete device'), 'External device removal must remain in connection settings')
  const result = document.getElementById('result')!; result.dataset.result = 'passed'
  result.textContent = 'PASS device deletion: incomplete inventory, settings, cancellation, pending state, unpairing, explicit local fallback, commissioning rejection, storage errors and Matter job polling'
}
void run().catch(error => { document.getElementById('result')!.textContent = String(error) })

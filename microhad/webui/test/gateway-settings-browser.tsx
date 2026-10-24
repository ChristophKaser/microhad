import { createRoot } from 'react-dom/client'
import { GatewaySettingsDialog } from '../src/GatewaySettingsDialog'

let state = { broadcast_passive_ack_quorum: 8, matter_country_code: 'DE', matter_regulatory_location: 'indoor', applied_value: 8 as number | null,
  application_state: 'applied', application_error: '' }
let rejectSave = false
let savedValue = 0
let savedCountry = ''
let savedLocation = ''
const networkCalls: { url: string; body: unknown }[] = []
window.fetch = async (_url, options) => {
  const url = String(_url)
  if (url === '/api/thread/create' || url === '/api/thread/recreate') {
    networkCalls.push({ url, body: JSON.parse(String(options?.body)) })
    return new Response(JSON.stringify({ schema: 1, ok: true, job: 1, state: 'queued' }), { status: 202 })
  }
  if (url === '/api/matter/jobs/1') return new Response(JSON.stringify({ job: 1, state: 'complete', response: {
    schema: 1, ok: true, result: { enabled: true, available: true, configured: true, attached: false, state: 'attaching', linux_ipv6_or_tun: false },
  } }), { status: 200 })
  if (options?.method === 'POST') {
    if (rejectSave) return new Response(JSON.stringify({ ok: false, error: 'Cannot save settings' }), { status: 400 })
    const fields = new URLSearchParams(String(options.body))
    if (fields.has('broadcast_passive_ack_quorum')) savedValue = Number(fields.get('broadcast_passive_ack_quorum'))
    savedCountry = fields.get('matter_country_code') || ''
    savedLocation = fields.get('matter_regulatory_location') || ''
    state = { ...state, broadcast_passive_ack_quorum: savedValue || state.broadcast_passive_ack_quorum,
      matter_country_code: savedCountry, matter_regulatory_location: savedLocation, application_state: 'pending' }
  }
  return new Response(JSON.stringify(state), { status: 200 })
}
const root = createRoot(document.getElementById('root')!)
root.render(<GatewaySettingsDialog onClose={() => root.render(<p>Closed</p>)} />)
const pause = () => new Promise(resolve => setTimeout(resolve, 50))
async function until(check: () => boolean) {
  for (let i = 0; i < 100; i++) { if (check()) return; await pause() }
  throw new Error('Timed out: ' + document.body.textContent)
}
function input(value: string) {
  const element = document.querySelector('input')!
  Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(element, value)
  element.dispatchEvent(new Event('input', { bubbles: true }))
}
function submit() { document.querySelector('form')!.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true })) }
async function run() {
  await until(() => document.querySelector('input')?.value === '8')
  if (document.body.textContent!.includes('Saved:') || document.body.textContent!.includes('Setting applied.')) throw new Error('Redundant settings status remains')
  if (document.body.textContent!.includes('Replace Zigbee network')) throw new Error('Replacement offered without an existing network')
  const selects = document.querySelectorAll('select')
  if (selects[0].value !== 'DE' || selects[1].value !== 'indoor') throw new Error('Germany/indoor defaults were not loaded')
  input('0'); await pause(); submit()
  await until(() => document.body.textContent!.includes('Enter an integer'))
  if (savedValue) throw new Error('Invalid value was saved')
  input('3'); await pause(); submit()
  await until(() => savedValue === 3 && document.body.textContent!.includes('awaiting Ziggurat'))
  input('4'); await pause()
  state = { ...state, applied_value: 3, application_state: 'applied' }
  await until(() => !document.body.textContent!.includes('awaiting Ziggurat'))
  if (document.querySelector('input')!.value !== '4') throw new Error('Polling overwrote the edit')
  rejectSave = true; submit()
  await until(() => document.body.textContent!.includes('Cannot save settings'))
  rejectSave = false
  state = { ...state, application_state: 'unsupported' }
  await until(() => document.querySelector('input')!.disabled)
  if ((document.querySelector('button[type="submit"]') as HTMLButtonElement).disabled) throw new Error('Region save disabled')
  selects[0].value = 'US'; selects[0].dispatchEvent(new Event('change', { bubbles: true }))
  selects[1].value = 'outdoor'; selects[1].dispatchEvent(new Event('change', { bubbles: true }))
  submit()
  await until(() => savedCountry === 'US' && savedLocation === 'outdoor')
  ;(document.querySelector('button[aria-label="Close"]') as HTMLButtonElement).click()
  await until(() => document.body.textContent!.includes('Closed'))
  let replacement = false
  root.render(<GatewaySettingsDialog onClose={() => {}} onReplaceZigbeeNetwork={() => { replacement = true }} />)
  await until(() => document.body.textContent!.includes('Replace Zigbee network'))
  const replace = [...document.querySelectorAll('button')].find(button => button.textContent === 'Replace Zigbee network')!
  if (replace.type !== 'button') throw new Error('Network replacement submits gateway settings')
  replace.click()
  if (!replacement) throw new Error('Network replacement action was not opened')
  root.render(<GatewaySettingsDialog key="matter-network" onClose={() => {}} matterNetworkConfigured={false} />)
  await until(() => document.body.textContent!.includes('Create Matter network'))
  ;[...document.querySelectorAll('button')].find(button => button.textContent === 'Create Matter network')!.click()
  await until(() => document.body.textContent!.includes('Recreate Matter network'))
  if (networkCalls.length !== 1 || networkCalls[0].url !== '/api/thread/create') throw new Error('Network create was not sent once')
  ;[...document.querySelectorAll('button')].find(button => button.textContent === 'Recreate Matter network')!.click()
  await until(() => document.body.textContent!.includes('Type RECREATE to confirm'))
  const recreate = [...document.querySelectorAll('button')].find(button => button.textContent === 'Recreate Matter network')!
  if (!recreate.disabled || networkCalls.length !== 1) throw new Error('Recreation bypasses confirmation')
  const confirmation = document.querySelector<HTMLInputElement>('fieldset input')!
  Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(confirmation, 'RECREATE')
  confirmation.dispatchEvent(new Event('input', { bubbles: true }))
  await until(() => !recreate.disabled)
  recreate.click()
  await until(() => networkCalls.length === 2 && !document.body.textContent!.includes('Type RECREATE to confirm'))
  if (networkCalls[1].url !== '/api/thread/recreate' || JSON.stringify(networkCalls[1].body) !== JSON.stringify({ schema: 1, confirmation: 'RECREATE' })) throw new Error('Confirmed network recreation request differs')
  const result = document.getElementById('result')!
  result.dataset.result = 'passed'; result.textContent = 'PASS gateway region defaults, save, threshold validation, polling, errors, unsupported and close'
}
void run().catch(error => { document.getElementById('result')!.textContent = String(error) })

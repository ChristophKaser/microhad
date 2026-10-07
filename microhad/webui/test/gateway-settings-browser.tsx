import { createRoot } from 'react-dom/client'
import { GatewaySettingsDialog } from '../src/GatewaySettingsDialog'

let state = { broadcast_passive_ack_quorum: 8, applied_value: 8 as number | null,
  application_state: 'applied', application_error: '' }
let rejectSave = false
let savedValue = 0
window.fetch = async (_url, options) => {
  if (options?.method === 'POST') {
    if (rejectSave) return new Response(JSON.stringify({ ok: false, error: 'Cannot save settings' }), { status: 400 })
    savedValue = Number(new URLSearchParams(String(options.body)).get('broadcast_passive_ack_quorum'))
    state = { ...state, broadcast_passive_ack_quorum: savedValue, application_state: 'pending' }
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
  input('0'); await pause(); submit()
  await until(() => document.body.textContent!.includes('Enter an integer'))
  if (savedValue) throw new Error('Invalid value was saved')
  input('3'); await pause(); submit()
  await until(() => savedValue === 3 && document.body.textContent!.includes('awaiting Ziggurat'))
  input('4'); await pause()
  state = { ...state, applied_value: 3, application_state: 'applied' }
  await until(() => document.body.textContent!.includes('Applied: 3'))
  if (document.querySelector('input')!.value !== '4') throw new Error('Polling overwrote the edit')
  rejectSave = true; submit()
  await until(() => document.body.textContent!.includes('Cannot save settings'))
  rejectSave = false
  state = { ...state, application_state: 'unsupported' }
  await until(() => document.querySelector('input')!.disabled)
  if (!(document.querySelector('button[type="submit"]') as HTMLButtonElement).disabled) throw new Error('Unsupported save enabled')
  ;(document.querySelector('button[aria-label="Close"]') as HTMLButtonElement).click()
  await until(() => document.body.textContent!.includes('Closed'))
  const result = document.getElementById('result')!
  result.dataset.result = 'passed'; result.textContent = 'PASS gateway settings loading, validation, save, polling, errors, unsupported and close'
}
void run().catch(error => { document.getElementById('result')!.textContent = String(error) })

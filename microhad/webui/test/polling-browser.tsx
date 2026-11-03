import { createRoot } from 'react-dom/client'
import { MatterPanel } from '../src/MatterPanel'
import { useGatewayData } from '../src/useGatewayData'
import { startVisiblePolling } from '../src/visiblePolling'

let hidden = false
Object.defineProperty(document, 'hidden', { configurable: true, get: () => hidden })
function visibility(value: boolean) { hidden = value; document.dispatchEvent(new Event('visibilitychange')) }
const pause = (ms = 30) => new Promise(resolve => setTimeout(resolve, ms))
function assert(value: unknown, message: string): asserts value { if (!value) throw new Error(message) }
async function until(check: () => boolean) {
  for (let i = 0; i < 100; i++) { if (check()) return; await pause() }
  throw new Error('Timed out')
}
const calls: string[] = []
window.fetch = async input => {
  const url = String(input); calls.push(url)
  const session = { session: '0000000000000001', state: 'waiting_thread', deadline_remaining_seconds: 100 }
  const value = url === '/api/matter/status' ? { enabled: true, available: true, controller_initialized: true, fabric_ready: true, state: 'running', commissioning_session: session.session }
    : url === '/api/thread/status' ? { enabled: true, available: true, configured: true, attached: true, state: 'attached', linux_ipv6_or_tun: false }
    : url === '/api/matter/commissioning/status' ? { schema: 1, ok: true, state: 'queued', job: 1 }
    : url === '/api/matter/jobs/1' ? { job: 1, state: 'complete', response: { schema: 1, ok: true, result: session } }
    : url === '/api/auth/status' ? { role: 2, passwords: [false, false, false] }
    : url.startsWith('/api/logs') ? { lines: ['123 test log'], next_before: 100, has_more: true }
    : { online: true }
  return new Response(JSON.stringify(value), { status: 200 })
}
let refreshStatus: () => Promise<void>
function Harness({ diagnostics }: { diagnostics: boolean }) {
  const data = useGatewayData(diagnostics); refreshStatus = data.refreshStatus
  return <div>{data.logs.join('\n')}</div>
}
const count = (prefix: string) => calls.filter(url => url.startsWith(prefix)).length
async function run() {
  let countPolls = 0, inFlight = 0, maxFlight = 0
  const stop = startVisiblePolling(async () => {
    countPolls++; inFlight++; maxFlight = Math.max(maxFlight, inFlight)
    await pause(60); inFlight--
    if (countPolls === 1) throw new Error('Expected rejection')
  }, 20)
  await pause(10); visibility(true); visibility(false); visibility(true); visibility(false)
  await until(() => countPolls >= 3)
  assert(maxFlight === 1, 'Slow polls overlap')
  visibility(true); await pause(90)
  const pausedCount = countPolls
  await pause(100); assert(countPolls === pausedCount, 'Hidden poll ran')
  visibility(false); assert(countPolls === pausedCount + 1, 'Resume did not refresh immediately')
  stop(); await pause(100)
  assert(countPolls === pausedCount + 1, 'Cleanup did not stop polling')

  const root = createRoot(document.getElementById('root')!)
  root.render(<Harness diagnostics />)
  await until(() => count('/api/logs') === 1 && document.body.textContent!.includes('test log'))
  root.render(<Harness diagnostics={false} />)
  await pause(50)
  const logs = count('/api/logs')
  await pause(2100)
  assert(count('/api/logs') === logs, 'Logs continued after leaving Diagnostics')
  visibility(true); await pause(50)
  const statuses = count('/api/status')
  await pause(2100)
  assert(count('/api/status') === statuses, 'Hidden status polling continued')
  await refreshStatus!()
  assert(count('/api/status') === statuses + 1, 'Explicit refresh was blocked while hidden')
  visibility(false)
  await until(() => count('/api/status') > statuses + 1)
  root.render(<Harness diagnostics />)
  await until(() => calls.includes('/api/logs?since=123'))
  assert(document.body.textContent!.includes('test log'), 'Logs were discarded on return')
  root.unmount(); await pause(50)
  const total = calls.length
  visibility(true); visibility(false); await pause(2100)
  assert(calls.length === total, 'Unmounted hook kept polling')
  const taskRoot = createRoot(document.getElementById('root')!)
  taskRoot.render(<MatterPanel />)
  await until(() => count('/api/matter/commissioning/status') > 0)
  visibility(true)
  const monitoring = count('/api/matter/commissioning/status')
  const availability = count('/api/matter/status')
  await until(() => count('/api/matter/commissioning/status') > monitoring)
  assert(count('/api/matter/status') === availability, 'Passive Matter availability continued while hidden')
  taskRoot.unmount()
  let hiddenMountCalls = 0
  const stopHidden = startVisiblePolling(() => { hiddenMountCalls++ }, 20)
  await pause(50); assert(hiddenMountCalls === 0, 'Hidden mount fetched passive data')
  stopHidden(); visibility(false)
  document.getElementById('result')!.textContent = 'PASS polling visibility, serialization, diagnostics cursors and cleanup'
  document.body.dataset.result = 'passed'
}
run().catch(error => { document.getElementById('result')!.textContent = String(error); document.body.dataset.result = 'failed' })

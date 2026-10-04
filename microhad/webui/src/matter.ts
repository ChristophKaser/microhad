/** Bounded Matter API client. No automatic mutation retries or response-body diagnostics. */
export type MatterSession = { session: string; state: string; deadline_remaining_seconds: number; node?: string; error?: string }
export type MatterStatus = { enabled: boolean; available: boolean; fabric_ready?: boolean; controller_initialized?: boolean; state?: string }
export type ThreadStatus = { enabled: boolean; available: boolean; configured?: boolean; attached?: boolean; state?: string; zigbee_channel?: number; dataset_channel?: number }
const states = ['waiting_ble', 'pase', 'provisioning', 'waiting_thread', 'case', 'completing', 'complete', 'cancelled', 'failed', 'timed_out']
const errors = ['malformed', 'unsupported', 'not_ready', 'busy', 'stale_session', 'stale_generation', 'not_found', 'timeout', 'cancelled', 'transport', 'storage', 'capacity', 'accessory', 'protocol', 'disabled']
export class MatterError extends Error { constructor(code: string) { super(errors.includes(code) ? code : 'protocol') } }
function object(value: unknown): Record<string, unknown> {
  if (!value || typeof value !== 'object' || Array.isArray(value)) throw new MatterError('protocol')
  return value as Record<string, unknown>
}
function integer(value: unknown, min: number, max: number): value is number {
  return typeof value === 'number' && Number.isInteger(value) && value >= min && value <= max
}
export function validSession(value: string): boolean { return /^[0-9a-f]{16}$/.test(value) && value !== '0000000000000000' }
export function validDataset(value: string): boolean { return /^(?:[0-9a-f]{2}){1,254}$/.test(value) }
export function sessionResult(value: unknown): MatterSession {
  const data = object(value)
  if (typeof data.session !== 'string' || !validSession(data.session) || typeof data.state !== 'string' ||
      !states.includes(data.state) || !integer(data.deadline_remaining_seconds, 0, 900) ||
      (data.node !== undefined && (typeof data.node !== 'string' || !validSession(data.node))) ||
      (data.error !== undefined && (typeof data.error !== 'string' || !errors.includes(data.error))) ||
      Object.keys(data).some(key => !['session', 'state', 'deadline_remaining_seconds', 'node', 'error'].includes(key))) throw new MatterError('protocol')
  return data as MatterSession
}
export function terminalSession(value: MatterSession): boolean { return ['complete', 'cancelled', 'failed', 'timed_out'].includes(value.state) }

async function request(url: string, body: unknown | undefined, signal: AbortSignal): Promise<Record<string, unknown>> {
  const controller = new AbortController()
  const abort = () => controller.abort()
  signal.addEventListener('abort', abort, { once: true })
  if (signal.aborted) controller.abort()
  const timer = setTimeout(abort, 3000)
  try {
    const response = await fetch(url, { method: body === undefined ? 'GET' : 'POST',
      ...(body === undefined ? {} : { body: JSON.stringify(body), headers: { 'Content-Type': 'application/json' } }),
      cache: 'no-store', redirect: 'error', signal: controller.signal })
    if (!response.body) throw new MatterError('protocol')
    const reader = response.body.getReader()
    const chunks: Uint8Array[] = []
    let length = 0
    try {
      for (;;) {
        const { done, value } = await reader.read()
        if (done) break
        length += value.length
        if (length > 2048) throw new MatterError('protocol')
        chunks.push(value)
      }
    } finally { await reader.cancel().catch(() => {}) }
    const bytes = new Uint8Array(length)
    let offset = 0
    for (const chunk of chunks) { bytes.set(chunk, offset); offset += chunk.length }
    const result = object(JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(bytes)))
    if (!response.ok || result.ok === false) throw new MatterError(typeof result.error === 'string' ? result.error : 'transport')
    return result
  } catch (error) {
    if (error instanceof MatterError) throw error
    throw new MatterError(signal.aborted ? 'cancelled' : controller.signal.aborted ? 'timeout' : 'transport')
  } finally { clearTimeout(timer); signal.removeEventListener('abort', abort) }
}

export function threadResult(value: unknown): ThreadStatus {
  const data = object(value)
  if (typeof data.enabled !== 'boolean' || typeof data.available !== 'boolean' ||
      typeof data.configured !== 'boolean' || typeof data.attached !== 'boolean' ||
      data.linux_ipv6_or_tun !== false || typeof data.state !== 'string' ||
      !['awaiting_coordinator', 'detached', 'attaching', 'attached', 'channel_mismatch', 'error'].includes(data.state) ||
      (data.attached && !data.configured) || Object.keys(data).some(key =>
        !['enabled', 'available', 'configured', 'attached', 'state', 'linux_ipv6_or_tun', 'zigbee_channel', 'dataset_channel'].includes(key))) throw new MatterError('protocol')
  for (const key of ['zigbee_channel', 'dataset_channel']) {
    if (data[key] !== undefined && !integer(data[key], 11, 26)) throw new MatterError('protocol')
  }
  return data as ThreadStatus
}
export async function matterAvailability(signal: AbortSignal): Promise<{ matter: MatterStatus; thread: ThreadStatus }> {
  const [matter, thread] = await Promise.all([request('/api/matter/status', undefined, signal), request('/api/thread/status', undefined, signal)])
  if (typeof matter.enabled !== 'boolean' || typeof matter.available !== 'boolean') throw new MatterError('protocol')
  if (!matter.enabled) {
    if (matter.available || Object.keys(matter).some(key => !['enabled', 'available'].includes(key))) throw new MatterError('protocol')
    return { matter: matter as MatterStatus, thread: { enabled: false, available: false } }
  }
  if (typeof matter.controller_initialized !== 'boolean' || typeof matter.fabric_ready !== 'boolean' ||
      !['running', 'awaiting_coordinator'].includes(String(matter.state)) || Object.keys(matter).some(key =>
        !['enabled', 'available', 'controller_initialized', 'fabric_ready', 'state'].includes(key))) throw new MatterError('protocol')
  return { matter: matter as MatterStatus, thread: threadResult(thread) }
}

function pause(ms: number, signal: AbortSignal): Promise<void> {
  return new Promise((resolve, reject) => {
    const abort = () => { clearTimeout(timer); signal.removeEventListener('abort', abort); reject(new MatterError('cancelled')) }
    const timer = setTimeout(() => { signal.removeEventListener('abort', abort); resolve() }, ms)
    signal.addEventListener('abort', abort, { once: true })
    if (signal.aborted) abort()
  })
}
export async function matterCall(route: string, fields: Record<string, unknown>, signal: AbortSignal): Promise<unknown> {
  if (!['/api/thread/create', '/api/thread/import', '/api/matter/commissioning/status', '/api/matter/commissioning/cancel'].includes(route)) throw new MatterError('unsupported')
  const keys = Object.keys(fields)
  if (route.endsWith('/create') && keys.length !== 0) throw new MatterError('malformed')
  if (route.endsWith('/import') && (keys.length !== 1 || typeof fields.dataset !== 'string' || !validDataset(fields.dataset))) throw new MatterError('malformed')
  if (route.includes('/commissioning/') && (keys.length !== 1 || typeof fields.session !== 'string' || !validSession(fields.session))) throw new MatterError('malformed')
  const controller = new AbortController()
  const abort = () => controller.abort()
  signal.addEventListener('abort', abort, { once: true })
  if (signal.aborted) abort()
  const timer = setTimeout(abort, 10000)
  try {
    const accepted = await request(route, { schema: 1, ...fields }, controller.signal)
    if (accepted.schema !== 1 || accepted.ok !== true || accepted.state !== 'queued' || !integer(accepted.job, 1, 0xffffffff)) throw new MatterError('protocol')
    for (;;) {
      const job = await request(`/api/matter/jobs/${accepted.job}`, undefined, controller.signal)
      if (job.job !== accepted.job) throw new MatterError('protocol')
      if (job.state === 'complete') {
        const reply = object(job.response)
        if (reply.schema !== 1 || typeof reply.ok !== 'boolean') throw new MatterError('protocol')
        if (!reply.ok) throw new MatterError(typeof reply.error === 'string' ? reply.error : 'protocol')
        return route.startsWith('/api/thread/') ? threadResult(reply.result) : reply.result
      }
      if (job.state === 'failed') throw new MatterError(typeof job.error === 'string' ? job.error : 'transport')
      if (!['queued', 'running'].includes(String(job.state))) throw new MatterError('protocol')
      await pause(200, controller.signal)
    }
  } catch (error) {
    if (controller.signal.aborted && !signal.aborted) throw new MatterError('timeout')
    throw error
  } finally { clearTimeout(timer); signal.removeEventListener('abort', abort) }
}

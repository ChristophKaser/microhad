import { useEffect, useRef, useState } from 'react'
import type { Device } from './api'
import { errorMessage, form, jsonRequest } from './api'

export function incompleteDevices(devices: Device[]): Device[] {
  const seen = new Set<string>()
  return devices.filter(device => {
    if (device.source !== 'zigbee' && device.source !== 'matter') return false
    const incomplete = device.setup_state === 'incomplete' || (!device.setup_state &&
      (device.source === 'matter' ? ['commissioning', 'recovery_required'].includes(device.commissioning_state ?? '') :
        device.endpoint_descriptors.length === 0))
    const key = device.source === 'matter' ? device.matter_node ?? device.id : device.id
    if (!incomplete || seen.has(key)) return false
    seen.add(key)
    return true
  })
}

export function IncompleteDevices({ devices, onDelete }: { devices: Device[]; onDelete: (device: Device) => void }) {
  const incomplete = incompleteDevices(devices)
  return <section className="panel">
    <div className="section-heading"><h2>Incomplete setup</h2></div>
    {incomplete.length === 0 ? <p className="muted">No incomplete devices.</p> : <div className="incomplete-devices">
      {incomplete.map(device => <div className="incomplete-device" key={device.id}>
        <div><strong>{device.name || device.id}</strong>
          <p className="muted small">{device.source === 'matter' ? 'Matter' : 'Zigbee'} · {device.commissioning_state === 'commissioning' ?
            'Commissioning in progress' : device.commissioning_state === 'recovery_required' ? 'Recovery required' : 'Interview incomplete'}</p>
          <code className="small">{device.matter_node ?? device.ieee_address}</code>
        </div>
        <button type="button" className="button danger" onClick={() => onDelete(device)} aria-label={`Delete ${device.name}`}>Delete</button>
      </div>)}
    </div>}
  </section>
}

type DeletionStatus = { state: string; error?: string; job?: number; response?: {
  ok: boolean; error?: string; result?: { state: string; error?: string }
} }
const deletionError = (value: string) => ({
  busy: 'Device setup or another operation is active. Cancel commissioning or wait for the operation to finish, then retry.',
  timeout: 'The device did not confirm unpairing within 15 seconds.',
  not_ready: 'The device or its network is unavailable.',
  accessory: 'The device could not confirm unpairing.',
  storage: 'Cannot save the device removal. Check storage and retry.',
  not_found: 'The device was not found. Refresh the device list and retry.',
  transport: 'The Matter service connection was lost. Refresh and retry.',
} as Record<string, string>)[value] ?? value

export function DeleteDeviceDialog({ device, onClose, onDeleted }: {
  device: Device; onClose: () => void; onDeleted: () => void
}) {
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState('')
  const [fallback, setFallback] = useState(false)
  const active = useRef(true)
  const operation = useRef<AbortController | null>(null)
  useEffect(() => { active.current = true; return () => { active.current = false; operation.current?.abort() } }, [])

  const remove = async (local: boolean) => {
    if (operation.current) return
    setBusy(true); setError(''); setFallback(false)
    const controller = new AbortController(); operation.current = controller
    const path = `/api/devices/${encodeURIComponent(device.id)}/delete`
    const timer = window.setTimeout(() => controller.abort(), 25000)
    try {
      const accepted = await jsonRequest<DeletionStatus>(path, { method: 'POST', body: form({ mode: local ? 'local' : 'unpair' }), signal: controller.signal })
      const deadline = Date.now() + 25000
      for (;;) {
        if (controller.signal.aborted) return
        const status = await jsonRequest<DeletionStatus>(accepted.job ? `/api/matter/jobs/${accepted.job}` : path, { signal: controller.signal })
        if (status.state === 'failed') throw new Error(status.error || 'Device deletion failed')
        if (accepted.job && status.state === 'complete') {
          const response = status.response
          if (!response?.ok) throw new Error(response?.error || 'Invalid deletion response')
          if (response.result?.state !== 'complete') throw new Error(response.result?.error || 'Device deletion did not complete')
          onDeleted(); return
        }
        if (!accepted.job && status.state === 'succeeded') { onDeleted(); return }
        if (!['pending', 'queued', 'running'].includes(status.state)) throw new Error('Invalid deletion response')
        if (Date.now() >= deadline) throw new Error('timeout')
        await new Promise(resolve => window.setTimeout(resolve, 200))
      }
    } catch (cause) {
      if (active.current) {
        const message = controller.signal.aborted ? 'timeout' : errorMessage(cause)
        setError(deletionError(message))
        setFallback(!local && !['busy', 'storage', 'not_found', 'malformed', 'unsupported'].includes(message))
      }
    } finally { window.clearTimeout(timer); operation.current = null; if (active.current) setBusy(false) }
  }

  return <div className="modal"><div className="dialog" role="dialog" aria-modal="true" aria-labelledby="delete-device-title">
    <div className="dialog-heading"><h2 id="delete-device-title">Delete device</h2>
      <button type="button" className="icon-button" disabled={busy} onClick={onClose} aria-label="Close">✕</button></div>
    <p>Delete <strong>{device.name || device.id}</strong>?</p>
    <p>This attempts to unpair the device and removes its settings, scene memberships, bindings and dashboard widgets from this gateway. Retained sensor history is kept.</p>
    {device.source === 'matter' && <p>All endpoints of this Matter device will be removed. Pairings with other controllers are kept.</p>}
    {busy && <p role="status">Deleting device… Waiting for confirmation.</p>}
    {error && <p className="error" role="alert">{error}</p>}
    {fallback && <p>Deleting locally leaves the physical device paired. It may need a factory reset before you can pair it again.</p>}
    <div className="dialog-actions">
      <button type="button" className="button" disabled={busy} onClick={onClose}>Cancel</button>
      {fallback && <button type="button" className="button danger" disabled={busy} onClick={() => void remove(true)}>Delete locally</button>}
      <button type="button" className="button danger" disabled={busy} onClick={() => void remove(false)}>{error ? 'Retry' : 'Unpair and delete'}</button>
    </div>
  </div></div>
}

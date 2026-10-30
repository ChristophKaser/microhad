import { useEffect, useState } from 'react'
import { authFetch } from './auth'
import type { Device } from './api'

type Status = { image: string; protocol: string; state: string; target: string; size: number; uploaded: number; progress: number; version: number; manufacturer: number; image_type: number; error: string }
async function request(action: string, body?: unknown): Promise<Status> {
  const response = await authFetch(`/api/ota/${action}`, body === undefined ? undefined : {
    method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body),
  })
  const value = await response.json()
  if (!response.ok) throw new Error(value.error || 'Firmware update request failed')
  return value
}
export function OtaUpdate({ device }: { device: Device }) {
  const [status, setStatus] = useState<Status>()
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState('')
  const [uploadPercent, setUploadPercent] = useState<number>()
  const target = device.source === 'matter' ? device.matter_node : device.ieee_address
  const active = status && ['starting', 'cancelling', 'waiting', 'transferring', 'awaiting_confirmation'].includes(status.state)
  useEffect(() => {
    let mounted = true
    const refresh = () => request('status').then(value => { if (mounted) setStatus(value) }).catch(cause => { if (mounted) setError(String(cause.message || cause)) })
    void refresh()
    const timer = window.setInterval(() => { if (!busy) void refresh() }, 2000)
    return () => { mounted = false; window.clearInterval(timer) }
  }, [busy])
  const upload = async (file: File) => {
    setBusy(true); setError(''); setUploadPercent(0)
    let image = ''
    try {
      if (file.size < 56 || file.size > 16 * 1024 * 1024) throw new Error('Choose an OTA firmware container up to 16 MiB.')
      let next = await request('begin', { size: file.size, protocol: device.source })
      image = next.image
      for (let offset = 0; offset < file.size; offset += 4096) {
        const bytes = new Uint8Array(await file.slice(offset, offset + 4096).arrayBuffer())
        const data = Array.from(bytes, byte => byte.toString(16).padStart(2, '0')).join('')
        next = await request('chunk', { image, offset, data })
        setUploadPercent(Math.floor(next.uploaded * 100 / file.size))
      }
      setStatus(await request('finish', { image }))
    } catch (cause) {
      setError(cause instanceof Error ? cause.message : String(cause))
      if (image) { try { setStatus(await request('discard', { image })) } catch { /* Keep the original error. */ } }
    } finally { setBusy(false); setUploadPercent(undefined) }
  }
  const act = async (action: string) => {
    setBusy(true); setError('')
    try { setStatus(await request(action, { image: status?.image, address: device.ieee_address, endpoint: device.endpoint_descriptors.find(endpoint => endpoint.input_clusters.includes(0x002a))?.endpoint ?? 0 })) }
    catch (cause) { setError(cause instanceof Error ? cause.message : String(cause)) }
    finally { setBusy(false) }
  }
  return <section aria-label="Firmware update">
    <h3>Firmware update</h3>
    <p className="muted">Upload a {device.source === 'matter' ? 'Matter' : 'Zigbee'} OTA firmware container from the device vendor. Files are temporary and disappear when the gateway reboots. Wake battery devices before starting.</p>
    <label className="field">Firmware file<input type="file" disabled={busy || Boolean(active) || status?.state === 'uploading'} onChange={event => {
      const file = event.target.files?.[0]; event.target.value = ''; if (file) void upload(file)
    }} /></label>
    {uploadPercent !== undefined && <p role="status">Uploading: {uploadPercent}%</p>}
    {status?.image && <>
      <p role="status">{status.protocol} firmware · {status.size.toLocaleString()} bytes · {status.state.replace(/_/g, ' ')}{status.target ? ` · ${status.target}` : ''}</p>
      {status.version > 0 && <p>Version: {status.version} · {status.protocol === 'matter' ? 'Vendor / product' : 'Manufacturer / image type'}: {status.manufacturer} / {status.image_type}</p>}
      {active && <progress aria-label="Firmware transfer" max={status.size} value={status.progress} />}
      {status.state === 'awaiting_confirmation' && <p>Firmware transferred. Waiting for the device to confirm its installed version.</p>}
      {active && status.target !== target && <p>An update is active for another device.</p>}
      <div className="actions">
        {status.state === 'ready' && status.protocol === device.source && <button type="button" disabled={busy} onClick={() => void act('start')}>Install on {device.name}</button>}
        {active && <button type="button" disabled={busy} onClick={() => void act('cancel')}>Cancel update</button>}
        {!active && <button type="button" disabled={busy} onClick={() => void act('discard')}>Discard firmware</button>}
      </div>
      {active && <p className="muted">Cancellation stops further transfer; it cannot undo an update already accepted by the device.</p>}
    </>}
    {(error || status?.error) && <p className="error" role="alert">{error || status?.error}</p>}
  </section>
}

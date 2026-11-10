import { FirmwareDownloads } from './FirmwareDownloads'
import { MatterFirmwareDownloads } from './MatterFirmwareDownloads'
import { startVisiblePolling } from './visiblePolling'
import { useEffect, useState } from 'react'
import { authFetch } from './auth'
import type { Device } from './api'

type Status = { image: string; protocol: string; state: string; target: string; size: number; uploaded: number; progress: number; version: number; manufacturer: number; image_type: number; error: string; mode?: 'upload' | 'stream'; requested_offset?: number; requested_length?: number }
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
  const [mode, setMode] = useState<'upload' | 'stream'>('upload')
  const [browserFile, setBrowserFile] = useState<{ file: File; image: string }>()
  const target = device.source === 'matter' ? device.matter_node : device.ieee_address
  const active = status && ['starting', 'cancelling', 'waiting', 'transferring', 'awaiting_confirmation'].includes(status.state)
  useEffect(() => {
    let mounted = true
    const refresh = () => request('status').then(value => { if (mounted) setStatus(value) }).catch(cause => { if (mounted) setError(String(cause.message || cause)) })
    const stop = startVisiblePolling(async () => { if (!busy) await refresh() }, 2000)
    return () => { mounted = false; stop() }
  }, [busy])
  useEffect(() => {
    if (!browserFile) return
    let stopped = false
    let timer: ReturnType<typeof setTimeout>
    const feed = async () => {
      try {
        const next = await request('status')
        if (stopped) return
        setStatus(next)
        if (next.image !== browserFile.image || ['empty', 'complete', 'cancelled', 'failed', 'awaiting_confirmation'].includes(next.state)) {
          setBrowserFile(undefined); return
        }
        const length = next.requested_length ?? 0
        const offset = next.requested_offset ?? 0
        if (next.mode === 'stream' && length > 0 && next.target === target) {
          if (length > 480 || offset < 0 || offset + length > browserFile.file.size) throw new Error('Invalid firmware stream range')
          const bytes = new Uint8Array(await browserFile.file.slice(offset, offset + length).arrayBuffer())
          if (stopped) return
          const data = Array.from(bytes, byte => byte.toString(16).padStart(2, '0')).join('')
          setStatus(await request('stream-chunk', { image: browserFile.image, offset, data }))
        }
      } catch (cause) {
        if (!stopped) setError(cause instanceof Error ? cause.message : String(cause))
      }
      if (!stopped) timer = setTimeout(() => void feed(), 100)
    }
    // Streaming must continue when the tab is hidden; visible polling is only for status.
    void feed()
    return () => { stopped = true; clearTimeout(timer) }
  }, [browserFile, target])
  const upload = async (file: File) => {
    setBusy(true); setError(''); setUploadPercent(0)
    let image = ''
    try {
      if (file.size < 56 || file.size > 16 * 1024 * 1024) throw new Error('Choose an OTA firmware container up to 16 MiB.')
      const streaming = device.source === 'matter' && mode === 'stream'
      let uploadSize = file.size
      if (streaming) {
        const prefix = new DataView(await file.slice(0, 16).arrayBuffer())
        const headerLength = prefix.getUint32(12, true)
        if (prefix.getUint32(0, true) !== 0x1beef11e || prefix.getUint32(4, true) !== file.size || prefix.getUint32(8, true) !== 0 || headerLength > 4096 || 16 + headerLength >= file.size) throw new Error('Invalid Matter OTA container')
        uploadSize = 16 + headerLength
      }
      setBrowserFile(undefined)
      let next = await request('begin', { size: file.size, protocol: device.source, mode: streaming ? 'stream' : 'upload', header_size: streaming ? uploadSize : undefined })
      image = next.image
      for (let offset = 0; offset < uploadSize; offset += 4096) {
        const bytes = new Uint8Array(await file.slice(offset, Math.min(offset + 4096, uploadSize)).arrayBuffer())
        const data = Array.from(bytes, byte => byte.toString(16).padStart(2, '0')).join('')
        next = await request('chunk', { image, offset, data })
        setUploadPercent(Math.floor(next.uploaded * 100 / uploadSize))
      }
      setStatus(await request('finish', { image }))
      if (streaming) setBrowserFile({ file, image })
    } catch (cause) {
      setError(cause instanceof Error ? cause.message : String(cause))
      if (image) { try { setStatus(await request('discard', { image })) } catch { /* Keep the original error. */ } }
    } finally { setBusy(false); setUploadPercent(undefined) }
  }
  const act = async (action: string) => {
    setBusy(true); setError('')
    try { setStatus(await request(action, { image: status?.image, address: device.id, endpoint: device.endpoint_descriptors.find(endpoint => endpoint.input_clusters.includes(0x002a))?.endpoint ?? 0 })) }
    catch (cause) { setError(cause instanceof Error ? cause.message : String(cause)) }
    finally { setBusy(false) }
  }
  return <section aria-label="Firmware update">
    <h3>Firmware update</h3>
    <p className="muted">Upload a {device.source === 'matter' ? 'Matter' : 'Zigbee'} OTA firmware container from the device vendor. Files are temporary and disappear when the gateway reboots. Wake battery devices before starting.</p>
    {device.source === 'zigbee' && <FirmwareDownloads device={device} />}
    {device.source === 'matter' && <MatterFirmwareDownloads device={device} />}
    {device.source === 'matter' && <label className="field">Transfer mode<select aria-label="Firmware transfer mode" value={mode} disabled={busy || Boolean(active) || status?.state === 'uploading'} onChange={event => setMode(event.target.value as 'upload' | 'stream')}>
      <option value="upload">Upload to gateway</option>
      <option value="stream">Stream from this browser</option>
    </select></label>}
    {device.source === 'matter' && mode === 'stream' && <p className="muted">Keep this page open until the transfer finishes. Only the header and a small chunk are buffered on the gateway. Firmware integrity is checked during transfer.</p>}
    <label className="field">Firmware file<input type="file" disabled={busy || Boolean(active) || status?.state === 'uploading'} onChange={event => {
      const file = event.target.files?.[0]; event.target.value = ''; if (file) void upload(file)
    }} /></label>
    {uploadPercent !== undefined && <p role="status">{mode === 'stream' ? 'Preparing' : 'Uploading'}: {uploadPercent}%</p>}
    {status?.image && <>
      <p role="status">{status.protocol} firmware · {status.size.toLocaleString()} bytes · {status.state.replace(/_/g, ' ')}{status.target ? ` · ${status.target}` : ''}</p>
      {status.mode === 'stream' && !browserFile && status.state === 'ready' && <p role="status">Select the firmware file again to attach this browser before installing.</p>}
      {status.version > 0 && <p>Version: {status.version} · {status.protocol === 'matter' ? 'Vendor / product' : 'Manufacturer / image type'}: {status.manufacturer} / {status.image_type}</p>}
      {active && <progress aria-label="Firmware transfer" max={status.size} value={status.progress} />}
      {status.state === 'awaiting_confirmation' && <p>Firmware transferred. Waiting for the device to confirm its installed version.</p>}
      {active && status.target !== target && <p>An update is active for another device.</p>}
      <div className="actions">
        {status.state === 'ready' && status.protocol === device.source && <button type="button" disabled={busy || (status.mode === 'stream' && browserFile?.image !== status.image)} onClick={() => void act('start')}>Install on {device.name}</button>}
        {active && <button type="button" disabled={busy} onClick={() => void act('cancel')}>Cancel update</button>}
        {!active && <button type="button" disabled={busy} onClick={() => void act('discard')}>Discard firmware</button>}
      </div>
      {active && <p className="muted">Cancellation stops further transfer; it cannot undo an update already accepted by the device.</p>}
    </>}
    {(error || status?.error) && <p className="error" role="alert">{error || status?.error}</p>}
  </section>
}

import { useEffect, useRef, useState } from 'react'
import { matterDevicePage } from './matter'
import type { MatterDevice } from './matter'

const features: Record<number, string> = { 6: 'Power', 8: 'Brightness', 1026: 'Temperature', 1029: 'Humidity', 1037: 'CO₂', 1066: 'PM2.5', 91: 'Air quality' }

export function MatterDevices({ available }: { available: boolean }) {
  const [devices, setDevices] = useState<MatterDevice[]>([])
  const [revision, setRevision] = useState<string>()
  const [next, setNext] = useState<number | null>(null)
  const [loaded, setLoaded] = useState(false)
  const [pending, setPending] = useState(false)
  const [error, setError] = useState('')
  const operation = useRef<AbortController | null>(null)
  useEffect(() => () => operation.current?.abort(), [])
  useEffect(() => { if (!available) { operation.current?.abort(); setPending(false) } }, [available])
  async function load(refresh: boolean) {
    if (operation.current || !available || (!refresh && next === null)) return
    const controller = new AbortController()
    operation.current = controller; setPending(true); setError('')
    try {
      const page = await matterDevicePage(refresh ? 0 : next!, refresh ? undefined : revision, controller.signal)
      if (controller.signal.aborted) return
      const combined = refresh ? page.devices : [...devices, ...page.devices]
      if (combined.length > 2048 || new Set(combined.map(device => `${device.node}:${device.endpoint}`)).size !== combined.length) throw new Error('protocol')
      if (!refresh && page.devices.length && devices.length) {
        const previous = devices[devices.length - 1], current = page.devices[0]
        if (current.node < previous.node || (current.node === previous.node && (current.endpoint ?? -1) <= (previous.endpoint ?? -1))) throw new Error('protocol')
      }
      setDevices(combined); setRevision(page.revision); setNext(page.next_cursor); setLoaded(true)
    } catch (cause) {
      if (!controller.signal.aborted) setError(cause instanceof Error && cause.message === 'stale_revision'
        ? 'The device list changed. Refresh it to continue.' : 'Could not read the device list. Refresh it to try again.')
    } finally {
      if (operation.current === controller) operation.current = null
      if (!controller.signal.aborted) setPending(false)
    }
  }
  return <div className="matter-devices">
    <div className="section-heading"><h3>Matter devices</h3>
      <button className="button" disabled={!available || pending} onClick={() => void load(true)}>Refresh devices</button></div>
    {error && <p role="alert" className="error">{error}{devices.length > 0 ? ' Previously loaded information is shown below.' : ''}</p>}
    {!available && <p className="muted">Gateway unavailable. Previously loaded information may be out of date.</p>}
    {!loaded && !pending && <p className="muted">Refresh to read the gateway’s Matter device list.</p>}
    {loaded && devices.length === 0 && <p>No Matter devices have been recorded.</p>}
    <ul className="matter-device-list">{devices.map(device => <li key={`${device.node}:${device.endpoint}`}>
      <strong>Node {device.node}{device.endpoint !== null ? ` · Endpoint ${device.endpoint}` : ''}</strong>
      <p>{device.state === 'recovery_required' ? 'Setup needs recovery' : device.state === 'commissioning' ? 'Setup incomplete' : device.endpoint === null ? 'Waiting for discovery' : device.online ? 'Available' : 'Offline'}</p>
      {device.servers.some(cluster => features[cluster]) && <p>Advertised features: {device.servers.filter(cluster => features[cluster]).map(cluster => features[cluster]).join(', ')}</p>}
      {device.id && <details><summary>Device ID</summary><code>{device.id}</code></details>}
    </li>)}</ul>
    {next !== null && <button className="button" disabled={!available || pending || Boolean(error)} onClick={() => void load(false)}>Load next endpoint</button>}
    {pending && <p role="status" aria-live="polite">Reading device list…</p>}
  </div>
}

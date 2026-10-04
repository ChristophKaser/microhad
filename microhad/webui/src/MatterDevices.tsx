import { useEffect, useRef, useState } from 'react'
import { matterDevicePage, matterLevel, matterOnOff } from './matter'
import type { MatterDevice } from './matter'

const features: Record<number, string> = { 6: 'Power', 8: 'Brightness', 1026: 'Temperature', 1029: 'Humidity', 1037: 'CO₂', 1066: 'PM2.5', 91: 'Air quality' }

export function MatterDevices({ available }: { available: boolean }) {
  const [devices, setDevices] = useState<MatterDevice[]>([])
  const [revision, setRevision] = useState<string>()
  const [next, setNext] = useState<number | null>(null)
  const [loaded, setLoaded] = useState(false)
  const [pending, setPending] = useState(false)
  const [error, setError] = useState('')
  const [power, setPower] = useState<Record<string, boolean>>({})
  const [level, setLevel] = useState<Record<string, number | null>>({})
  const [stagedLevel, setStagedLevel] = useState<Record<string, number>>({})
  const [transition, setTransition] = useState<Record<string, number>>({})
  const [operationError, setOperationError] = useState<Record<string, string>>({})
  const [activeDevice, setActiveDevice] = useState<string | null>(null)
  const [activeKind, setActiveKind] = useState<'power' | 'level' | null>(null)
  const operation = useRef<AbortController | null>(null)
  useEffect(() => () => operation.current?.abort(), [])
  useEffect(() => { if (!available) { operation.current?.abort(); setPending(false); setActiveDevice(null); setActiveKind(null) } }, [available])
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
  async function operate(device: MatterDevice, action: 'read' | 'on' | 'off' | 'toggle') {
    if (!available || pending || operation.current || !device.online || device.endpoint === null || activeDevice !== null) return
    const key = `${device.node}:${device.endpoint}`
    const controller = new AbortController()
    operation.current = controller; setActiveDevice(key); setActiveKind('power'); setOperationError(current => ({ ...current, [key]: '' }))
    try {
      const value = await matterOnOff(device.node, device.endpoint, action, controller.signal)
      if (!controller.signal.aborted) setPower(current => ({ ...current, [key]: value }))
    } catch (cause) {
      if (!controller.signal.aborted) setOperationError(current => ({ ...current, [key]: cause instanceof Error ? cause.message : 'protocol' }))
    } finally {
      if (operation.current === controller) operation.current = null
      if (!controller.signal.aborted) { setActiveDevice(null); setActiveKind(null) }
    }
  }
  async function operateLevel(device: MatterDevice, action: 'read' | 'move') {
    if (!available || pending || operation.current || !device.online || device.endpoint === null || activeDevice !== null) return
    const key = `${device.node}:${device.endpoint}`
    const controller = new AbortController()
    const selected = stagedLevel[key] ?? level[key] ?? 0
    const transitionDeciseconds = transition[key] ?? 0
    operation.current = controller; setActiveDevice(key); setActiveKind('level')
    setOperationError(current => ({ ...current, [`${key}:level`]: '' }))
    try {
      const value = await matterLevel(device.node, device.endpoint, action === 'read' ? { kind: 'read' } : {
        kind: 'move', level: selected, transitionDeciseconds,
      }, controller.signal)
      if (!controller.signal.aborted) {
        setLevel(current => ({ ...current, [key]: value }))
        if (value !== null) setStagedLevel(current => ({ ...current, [key]: value }))
      }
    } catch (cause) {
      if (!controller.signal.aborted) setOperationError(current => ({ ...current,
        [`${key}:level`]: cause instanceof Error ? cause.message : 'protocol' }))
    } finally {
      if (operation.current === controller) operation.current = null
      if (!controller.signal.aborted) { setActiveDevice(null); setActiveKind(null) }
    }
  }
  return <div className="matter-devices">
    <div className="section-heading"><h3>Matter devices</h3>
      <button className="button" disabled={!available || pending || activeDevice !== null} onClick={() => void load(true)}>Refresh devices</button></div>
    {error && <p role="alert" className="error">{error}{devices.length > 0 ? ' Previously loaded information is shown below.' : ''}</p>}
    {!available && <p className="muted">Gateway unavailable. Previously loaded information may be out of date.</p>}
    {!loaded && !pending && <p className="muted">Refresh to read the gateway’s Matter device list.</p>}
    {loaded && devices.length === 0 && <p>No Matter devices have been recorded.</p>}
    <ul className="matter-device-list">{devices.map(device => <li key={`${device.node}:${device.endpoint}`}>
      <strong>Node {device.node}{device.endpoint !== null ? ` · Endpoint ${device.endpoint}` : ''}</strong>
      <p>{device.state === 'recovery_required' ? 'Setup needs recovery' : device.state === 'commissioning' ? 'Setup incomplete' : device.endpoint === null ? 'Waiting for discovery' : device.online ? 'Available' : 'Offline'}</p>
      {device.servers.some(cluster => features[cluster]) && <p>Advertised features: {device.servers.filter(cluster => features[cluster]).map(cluster => features[cluster]).join(', ')}</p>}
      {device.endpoint !== null && device.servers.includes(6) && <div className="matter-device-controls" aria-label={`OnOff controls for node ${device.node} endpoint ${device.endpoint}`}>
        <p>Power: {power[`${device.node}:${device.endpoint}`] === undefined ? 'Not read' : power[`${device.node}:${device.endpoint}`] ? 'On' : 'Off'}</p>
        {(['read', 'on', 'off', 'toggle'] as const).map(action => <button key={action} className="button" disabled={!available || pending || !device.online || activeDevice !== null}
          onClick={() => void operate(device, action)}>{action === 'read' ? 'Read power' : action === 'on' ? 'Turn on' : action === 'off' ? 'Turn off' : 'Toggle'}</button>)}
        {operationError[`${device.node}:${device.endpoint}`] && <p role="alert" className="error">Power operation failed ({operationError[`${device.node}:${device.endpoint}`]}). Read the device again to check its current state.</p>}
        {activeDevice === `${device.node}:${device.endpoint}` && activeKind === 'power' && <p role="status" aria-live="polite">Waiting for confirmed power state…</p>}
      </div>}
      {device.endpoint !== null && device.servers.includes(8) && <div className="matter-device-controls" aria-label={`Level Control for node ${device.node} endpoint ${device.endpoint}`}>
        <p>Brightness: {level[`${device.node}:${device.endpoint}`] === undefined ? 'Not read' : level[`${device.node}:${device.endpoint}`] === null ? 'Unknown' : `${level[`${device.node}:${device.endpoint}`]}/254`}</p>
        <label>Brightness level (0–254)
          <input type="range" min="0" max="254" step="1" aria-label={`Brightness level for node ${device.node} endpoint ${device.endpoint}`}
            value={stagedLevel[`${device.node}:${device.endpoint}`] ?? level[`${device.node}:${device.endpoint}`] ?? 0}
            disabled={!available || pending || !device.online || activeDevice !== null}
            onChange={event => { const value = Number(event.currentTarget.value); setStagedLevel(current => ({ ...current, [`${device.node}:${device.endpoint}`]: value })) }} />
        </label>
        <label>Transition (deciseconds)
          <input type="number" min="0" max="65535" step="1" aria-label={`Transition deciseconds for node ${device.node} endpoint ${device.endpoint}`}
            value={transition[`${device.node}:${device.endpoint}`] ?? 0}
            disabled={!available || pending || !device.online || activeDevice !== null}
            onChange={event => { const value = Number(event.currentTarget.value); setTransition(current => ({ ...current, [`${device.node}:${device.endpoint}`]: value })) }} />
        </label>
        <button className="button" disabled={!available || pending || !device.online || activeDevice !== null}
          onClick={() => void operateLevel(device, 'read')}>Read brightness</button>
        <button className="button" disabled={!available || pending || !device.online || activeDevice !== null}
          onClick={() => void operateLevel(device, 'move')}>Set brightness</button>
        {operationError[`${device.node}:${device.endpoint}:level`] && <p role="alert" className="error">Brightness operation failed ({operationError[`${device.node}:${device.endpoint}:level`]}). Read the device again to check its current level.</p>}
        {activeDevice === `${device.node}:${device.endpoint}` && activeKind === 'level' && <p role="status" aria-live="polite">Waiting for confirmed brightness level…</p>}
      </div>}
      {device.id && <details><summary>Device ID</summary><code>{device.id}</code></details>}
    </li>)}</ul>
    {next !== null && <button className="button" disabled={!available || pending || activeDevice !== null || Boolean(error)} onClick={() => void load(false)}>Load next endpoint</button>}
    {pending && <p role="status" aria-live="polite">Reading device list…</p>}
  </div>
}

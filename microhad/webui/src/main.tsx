import { useEffect, useState } from 'react'
import { createRoot } from 'react-dom/client'
import type { Device, EndpointDescriptor, Status } from './api'
import { errorMessage, form, jsonRequest, post, textRequest } from './api'
import { SceneEditor } from './SceneEditor'
import './style.css'

const profileNames: Record<number, string> = { 260: 'Home Automation', 49246: 'Zigbee Light Link', 41440: 'Green Power' }
const deviceNames: Record<number, string> = { 0: 'On/Off Switch', 256: 'On/Off Light', 257: 'Dimmable Light', 770: 'Temperature Sensor', 775: 'Humidity Sensor', 1026: 'IAS Zone' }
const clusterNames: Record<number, string> = { 0: 'Basic', 1: 'Power Configuration', 3: 'Identify', 4: 'Groups', 5: 'Scenes', 6: 'On/Off', 8: 'Level Control', 25: 'Over-the-Air Upgrade', 32: 'Poll Control', 768: 'Color Control', 1024: 'Illuminance Measurement', 1026: 'Temperature Measurement', 1027: 'Pressure Measurement', 1029: 'Relative Humidity', 1030: 'Occupancy Sensing', 1280: 'IAS Zone', 1794: 'Simple Metering', 1795: 'Messaging', 1796: 'Tunneling', 2817: 'BACnet Protocol Tunnel', 2819: 'Appliance Events and Alerts', 2820: 'Electrical Measurement' }
const namedId = (id: number, names: Record<number, string>) => `0x${id.toString(16).padStart(4, '0').toUpperCase()}${names[id] ? ` — ${names[id]}` : ''}`
const dimmable = (device: Device) => device.has_brightness || device.endpoint_descriptors.some(endpoint => endpoint.input_clusters.includes(8))
const icon = (d: Device) => d.has_light ? '💡' : d.has_remote ? '🎛️' : d.has_contact ? '🚪' : d.has_temperature ? '🌡️' : d.has_humidity ? '💧' : d.has_occupancy ? '👁️' : '📡'
function relativeTime(epoch: number): string {
  if (!epoch) return 'Never'
  const seconds = Math.max(0, Math.floor(Date.now() / 1000 - epoch))
  if (seconds < 15) return 'just now'
  if (seconds < 60) return `${seconds} sec ago`
  if (seconds < 3600) return `${Math.floor(seconds / 60)} min ago`
  if (seconds < 86400) return `${Math.floor(seconds / 3600)} hr ago`
  return `${Math.floor(seconds / 86400)} days ago`
}
function ClipboardButton({ text }: { text: string }) {
  const [copied, setCopied] = useState(false)
  const copy = async () => {
    try {
      if (navigator.clipboard?.writeText) await navigator.clipboard.writeText(text)
      else { const input = document.createElement('textarea'); input.value = text; document.body.appendChild(input); input.select(); document.execCommand('copy'); input.remove() }
      setCopied(true)
    } catch (e) { window.alert(errorMessage(e)) }
  }
  return <button className="button" onClick={copy}>{copied ? '✓ Copied' : '⧉ Copy device info'}</button>
}
function DescriptorList({ descriptor }: { descriptor: EndpointDescriptor }) {
  return <section className="descriptor-row"><h3>Endpoint {descriptor.endpoint}</h3><p className="mono muted">Profile {namedId(descriptor.profile_id, profileNames)} · Device {namedId(descriptor.device_id, deviceNames)} · Version {descriptor.device_version}</p><div className="descriptor-columns"><div><b>Server / input clusters</b><ul>{descriptor.input_clusters.map(id => <li key={id}>{namedId(id, clusterNames)}</li>)}{!descriptor.input_clusters.length && <li>None</li>}</ul></div><div><b>Client / output clusters</b><ul>{descriptor.output_clusters.map(id => <li key={id}>{namedId(id, clusterNames)}</li>)}{!descriptor.output_clusters.length && <li>None</li>}</ul></div></div></section>
}
function DeviceCard({ device, action, onSettings, onDescriptors }: { device: Device; action: (url: string, body?: string) => void; onSettings: () => void; onDescriptors: () => void }) {
  const [brightness, setBrightness] = useState(device.has_brightness ? device.brightness_percent : 100)
  useEffect(() => setBrightness(device.has_brightness ? device.brightness_percent : 100), [device.brightness_percent, device.has_brightness])
  const url = `/api/devices/${encodeURIComponent(device.ieee_address)}`
  const metrics = [device.has_light && `💡 ${device.light_on ? 'On' : 'Off'}`, device.has_contact && `${device.contact_open ? '🔓 Open' : '🔒 Closed'}`, device.has_temperature && `🌡️ ${device.temperature_celsius} °C`, device.has_humidity && `💧 ${device.humidity_percent}%`, device.has_occupancy && `👁️ ${device.occupied ? 'Occupied' : 'Clear'}`, device.has_battery && `🔋 ${device.battery_percent}%`, `📶 LQI ${device.link_quality}`].filter(Boolean)
  return <article className="device-card"><div className="device-head"><span className="device-icon">{icon(device)}</span><div className="grow"><h3>{device.name}</h3><p className="muted small">{device.kind}</p></div><div className="mini-actions"><button title="Inspect descriptors" aria-label={`Inspect descriptors for ${device.name}`} onClick={onDescriptors}>⌕</button><button title="Settings" aria-label={`Settings for ${device.name}`} onClick={onSettings}>⚙</button><button title="Refresh attributes" aria-label={`Refresh ${device.name}`} onClick={() => action(`${url}/refresh`)}>↻</button></div></div><div className="metrics">{metrics.map((metric, index) => <span className="metric" key={index}>{metric}</span>)}</div>
    {device.has_light && <div className="light-panel"><div className="light-row"><button className={`button ${device.light_on ? 'primary' : ''}`} onClick={() => action(`${url}/light`, form({ state: device.light_on ? 'off' : 'on' }))}>{device.light_on ? '⏻ Turn off' : '⏻ Turn on'}</button>{dimmable(device) && <><input type="range" min="0" max="100" value={brightness} aria-label={`Brightness for ${device.name}`} onChange={event => setBrightness(Number(event.target.value))} onPointerUp={event => action(`${url}/light`, form({ brightness: event.currentTarget.value }))} onKeyUp={event => { if (['ArrowLeft', 'ArrowRight', 'Home', 'End', 'PageUp', 'PageDown', 'Enter'].includes(event.key)) action(`${url}/light`, form({ brightness: event.currentTarget.value })) }} /><span>{brightness}%</span></>}</div>{dimmable(device) && <div className="presets">{[25, 50, 75, 100].map(level => <button className="button" key={level} onClick={() => action(`${url}/light`, form({ brightness: level }))}>{level}%</button>)}</div>}</div>}
    <div className="device-foot"><span title={device.last_seen ? new Date(device.last_seen * 1000).toLocaleString() : undefined}>Last contact: {relativeTime(device.last_seen)}</span><span className="mono">{device.ieee_address}</span></div>{(device.manufacturer_name || device.model_id) && <p className="muted small identity">{[device.manufacturer_name && `Vendor: ${device.manufacturer_name}`, device.model_id && `Model: ${device.model_id}`].filter(Boolean).join(' · ')}</p>}</article>
}
function DeviceSettings({ device, onClose, onSaved }: { device: Device; onClose: () => void; onSaved: () => void }) {
  const [name, setName] = useState(device.name)
  const [low, setLow] = useState(device.has_low_temperature_alert ? String(device.low_temperature_alert) : '')
  const [high, setHigh] = useState(device.has_high_temperature_alert ? String(device.high_temperature_alert) : '')
  const [initial, setInitial] = useState(String(device.contact_initial_alert_minutes))
  const [reminder, setReminder] = useState(String(device.contact_reminder_alert_minutes))
  const [refresh, setRefresh] = useState(String(device.sensor_refresh_interval_minutes))
  const [error, setError] = useState('')
  const [saving, setSaving] = useState(false)
  const save = async (event: React.FormEvent) => {
    event.preventDefault(); setSaving(true); setError('')
    const fields: Record<string, string> = { name }
    if (device.has_temperature) { fields.low = low; fields.high = high }
    if (device.has_contact) { fields.initial_minutes = initial; fields.reminder_minutes = reminder }
    if (device.has_temperature || device.has_humidity) fields.sensor_refresh_minutes = refresh
    try { await post(`/api/devices/${encodeURIComponent(device.ieee_address)}/settings`, form(fields)); onSaved(); onClose() } catch (e) { setError(errorMessage(e)) } finally { setSaving(false) }
  }
  return <div className="modal"><form className="dialog" onSubmit={save}><div className="dialog-heading"><h2>Device settings</h2><button type="button" className="icon-button" onClick={onClose} aria-label="Close">✕</button></div><p className="muted">{device.name} · {device.ieee_address}</p>{error && <p className="error" role="alert">{error}</p>}
    <label className="field">Name<input value={name} onChange={event => setName(event.target.value)} maxLength={64} required /></label>
    {device.has_contact && <><label className="field">First alert after (minutes)<input type="number" min="1" step="1" value={initial} onChange={event => setInitial(event.target.value)} required /></label><label className="field">Reminder every (minutes)<input type="number" min="1" step="1" value={reminder} onChange={event => setReminder(event.target.value)} required /></label></>}
    {device.has_temperature && <><label className="field">Low threshold °C (blank disables)<input type="number" step=".1" value={low} onChange={event => setLow(event.target.value)} /></label><label className="field">High threshold °C (blank disables)<input type="number" step=".1" value={high} onChange={event => setHigh(event.target.value)} /></label></>}
    {(device.has_temperature || device.has_humidity) && <label className="field">Fallback refresh interval (minutes; 0 disables)<input type="number" min="0" step="1" value={refresh} onChange={event => setRefresh(event.target.value)} required /></label>}
    <div className="dialog-actions"><button type="button" className="button" onClick={onClose}>Cancel</button><button className="button primary" disabled={saving}>{saving ? 'Saving…' : 'Save settings'}</button></div></form></div>
}
function LogLine({ line }: { line: string }) {
  const match = line.match(/^(\d+)\s+(SERIAL|EZSP|APS|FRAME)\b\s*(.*)$/)
  if (!match) return <span className="log-line">{line}</span>
  const [, epoch, tag, body] = match
  const time = new Date(Number(epoch) * 1000)
  const timestamp = time.toDateString() === new Date().toDateString() ? time.toLocaleTimeString() : time.toLocaleString()
  const ieee = tag === 'FRAME' ? body.match(/ieee=(0x[0-9a-f]+)/i)?.[1] : undefined
  const hue = ieee ? [...ieee].reduce((hash, char) => (hash * 31 + char.charCodeAt(0)) >>> 0, 0) % 360 : undefined
  return <span className={`log-line log-${tag.toLowerCase()}`} style={hue === undefined ? undefined : { color: `hsl(${hue} 72% 72%)` }}><span className="log-time" title={`Unix timestamp: ${epoch}`}>{timestamp}</span> <b className="log-tag">{tag}</b> {body}</span>
}
function App() {
  const [status, setStatus] = useState<Status | null>(null)
  const [statusError, setStatusError] = useState('')
  const [logs, setLogs] = useState<string[]>([])
  const [logsError, setLogsError] = useState('')
  const [dialog, setDialog] = useState<'network' | 'scenes' | 'text' | null>(null)
  const [selected, setSelected] = useState<Device | null>(null)
  const [descriptors, setDescriptors] = useState<Device | null>(null)
  const [confirmation, setConfirmation] = useState('')
  const [dialogError, setDialogError] = useState('')
  const [sceneText, setSceneText] = useState('')
  const [busy, setBusy] = useState(false)
  const [restarting, setRestarting] = useState(false)
  const refresh = async () => { try { setStatus(await jsonRequest<Status>('/api/status')); setStatusError('') } catch (e) { setStatusError(errorMessage(e)) } }
  const refreshLogs = async () => { try { setLogs((await jsonRequest<{ lines: string[] }>('/api/logs')).lines); setLogsError('') } catch (e) { setLogsError(errorMessage(e)) } }
  useEffect(() => { void refresh(); void refreshLogs(); const timer = window.setInterval(() => { void refresh(); void refreshLogs() }, 2000); return () => window.clearInterval(timer) }, [])
  const action = async (url: string, body = '') => { try { const result = await post(url, body) as { message?: string }; if (result?.message) window.alert(result.message); await refresh() } catch (e) { window.alert(errorMessage(e)) } }
  const openText = async () => { setDialogError(''); setDialog('text'); try { setSceneText(await textRequest('/api/scenes/config')) } catch (e) { setDialogError(errorMessage(e)) } }
  const saveText = async () => { setBusy(true); setDialogError(''); try { await post('/api/scenes/config', sceneText); setDialog(null); await refresh() } catch (e) { setDialogError(errorMessage(e)) } finally { setBusy(false) } }
  const createNetwork = async (event: React.FormEvent) => { event.preventDefault(); setBusy(true); setDialogError(''); try { await post('/api/network/create', form({ confirmation })); setDialog(null); await refresh() } catch (e) { setDialogError(errorMessage(e)) } finally { setBusy(false) } }
  const restart = async () => { if (!window.confirm('Restart the Zigbee service? Joining will close briefly.')) return; try { await post('/api/service/restart'); setRestarting(true); window.setTimeout(() => { setRestarting(false); void refresh() }, 3000) } catch (e) { window.alert(errorMessage(e)) } }
  const enablePush = async () => { if (import.meta.env.DEV) return; if (!('serviceWorker' in navigator) || !('PushManager' in window)) { window.alert('Push is unavailable in this browser'); return } try { const registration = await navigator.serviceWorker.register('/sw.js'); const key = await jsonRequest<{ public_key: string }>('/api/push/vapid-key'); const base64 = key.public_key.replace(/-/g, '+').replace(/_/g, '/').padEnd(Math.ceil(key.public_key.length / 4) * 4, '='); const bytes = Uint8Array.from(atob(base64), char => char.charCodeAt(0)); const subscription = await registration.pushManager.subscribe({ userVisibleOnly: true, applicationServerKey: bytes }); await post('/api/push/subscriptions', JSON.stringify(subscription)); window.alert('Browser alerts enabled') } catch (e) { window.alert(`Could not enable alerts: ${errorMessage(e)}`) } }
  const now = Math.floor(Date.now() / 1000), joinLeft = Math.max(0, (status?.joining_until ?? 0) - now)
  const networkState = status?.network_formed ? 'Formed' : status?.network_forming ? 'Forming' : 'Not formed'
  return <main className="shell"><header className="hero"><div><p className="eyebrow">Smart home / Gateway</p><h1>MicroHAD</h1><p className="hero-sub">Devices, scenes, and coordinator activity in one place.</p></div><div className="serial-state">{restarting ? 'Restarting service…' : statusError || (status ? `Serial RX: ${status.serial_rx_bytes} bytes · ${status.ash_rx_frames} frames` : 'Connecting…')}</div></header>
    <section className="overview"><div className={`overview-item ${statusError || !status?.online ? 'bad' : 'good'}`}><span className="overview-icon">●</span><div><span className="overview-label">Coordinator</span><strong>{statusError ? 'Unavailable' : status?.online ? 'Online' : 'Offline'}</strong></div></div><div className={`overview-item ${status?.network_formed ? 'good' : status?.network_forming ? 'warn' : ''}`}><span className="overview-icon">⌘</span><div><span className="overview-label">Zigbee network</span><strong>{networkState}</strong></div></div><div className="overview-item"><span className="overview-icon">▦</span><div><span className="overview-label">Paired devices</span><strong>{status?.devices.length ?? '—'}</strong></div></div></section>
    <section className="panel"><div className="section-heading"><div><p className="eyebrow">Control center</p><h2>Network & alerts</h2></div><p className="muted">{joinLeft ? `Joining enabled: pair a sensor within ${Math.ceil(joinLeft / 60)} minute(s).` : status?.joining_preparing ? 'Preparing secure pairing…' : 'Joining is closed.'}</p></div><div className="toolbar"><button className="button primary" disabled={status?.joining_preparing} onClick={() => action('/api/join')}>{joinLeft ? 'Extend joining (5 min)' : status?.joining_preparing ? 'Preparing…' : 'Allow joining (5 min)'}</button><button className="button danger" onClick={() => { setConfirmation(''); setDialogError(''); setDialog('network') }}>Create fresh network</button><button className="button" onClick={restart}>Restart service</button><button className="button" onClick={enablePush} disabled={import.meta.env.DEV}>Enable browser alerts</button><button className="button" disabled={!status?.notifications_enabled} onClick={() => action('/api/notifications/test')}>Test alerts</button></div><div className="toggle-row"><label className="check"><input type="checkbox" checked={!!status?.notifications_enabled} onChange={event => action('/api/notifications/enabled', form({ enabled: String(event.target.checked) }))} /> Push notifications</label>{import.meta.env.DEV && <span className="muted small">Browser-push enrollment is disabled in local development.</span>}</div></section>
    <section className="content-section"><div className="section-heading"><div><p className="eyebrow">Connected home</p><h2>Devices</h2></div></div><div className="device-grid">{status?.devices.length ? status.devices.map(device => <DeviceCard key={device.ieee_address} device={device} action={action} onSettings={() => setSelected(device)} onDescriptors={() => setDescriptors(device)} />) : <div className="empty">No Zigbee devices discovered yet.</div>}</div></section>
    <section className="panel"><div className="section-heading"><div><p className="eyebrow">Lighting</p><h2>Scenes & groups</h2></div><div className="toolbar"><button className="button primary" onClick={() => setDialog('scenes')}>Configure scenes & groups</button><button className="button" onClick={openText}>Advanced text editor</button></div></div><p className="muted">Native-capable bulbs are recalled together with Zigbee Groups and Scenes; unsupported bulbs use a direct fallback.</p><div className="scene-grid">{status?.scene_groups.length ? status.scene_groups.map(group => <article className="scene-summary" key={group.id}><h3>{group.name}</h3><p className="muted small">Zigbee group {group.id} · {group.members.length} light(s)</p><div className="presets">{group.scenes.map(scene => <button key={scene.id} className="button" onClick={() => action(`/api/scenes/${group.id}/${scene.id}/activate`)}>▶ {scene.name}</button>)}</div><button className="button danger" onClick={() => action(`/api/scenes/${group.id}/off`)}>⏻ Turn group off</button></article>) : <div className="empty">No scene groups configured yet.</div>}</div></section>
    <section className="panel"><details open><summary><span className="eyebrow">Diagnostics</span><h2>Live coordinator log</h2></summary><pre className="logs">{logsError ? `Unable to load logs: ${logsError}` : logs.map((line, index) => <LogLine key={index} line={line} />)}</pre></details></section>
    {selected && <DeviceSettings key={selected.ieee_address} device={selected} onClose={() => setSelected(null)} onSaved={() => void refresh()} />}
    {descriptors && <div className="modal"><section className="dialog wide" role="dialog" aria-modal="true" aria-labelledby="descriptor-title"><div className="dialog-heading"><div><p className="eyebrow">Device details</p><h2 id="descriptor-title">Stored descriptors</h2></div><button className="icon-button" onClick={() => setDescriptors(null)} aria-label="Close">✕</button></div><p className="muted">{descriptors.name} · {descriptors.ieee_address}</p>{descriptors.endpoint_descriptors.length ? descriptors.endpoint_descriptors.map(item => <DescriptorList key={item.endpoint} descriptor={item} />) : <p className="muted">No endpoint descriptors have been saved for this device yet.</p>}<div className="dialog-actions"><ClipboardButton text={[`IEEE / EUI-64: ${descriptors.ieee_address}`, descriptors.manufacturer_name && `Vendor: ${descriptors.manufacturer_name}`, descriptors.model_id && `Model: ${descriptors.model_id}`].filter(Boolean).join('\n')} /><button className="button" onClick={() => setDescriptors(null)}>Close</button></div></section></div>}
    {dialog === 'scenes' && <SceneEditor devices={status?.devices ?? []} onClose={() => setDialog(null)} onSaved={() => void refresh()} />}
    {dialog === 'text' && <div className="modal"><section className="dialog wide" role="dialog" aria-modal="true" aria-labelledby="text-title"><div className="dialog-heading"><h2 id="text-title">Advanced scene text editor</h2><button className="icon-button" onClick={() => setDialog(null)} aria-label="Close">✕</button></div><p className="muted">Saving here replaces all scene and group settings.</p>{dialogError && <p className="error" role="alert">{dialogError}</p>}<textarea className="advanced-text" spellCheck={false} value={sceneText} onChange={event => setSceneText(event.target.value)} /><div className="dialog-actions"><button className="button" onClick={() => setDialog(null)}>Cancel</button><button className="button primary" disabled={busy} onClick={saveText}>{busy ? 'Saving…' : 'Save & sync scenes'}</button></div></section></div>}
    {dialog === 'network' && <div className="modal"><form className="dialog" onSubmit={createNetwork}><div className="dialog-heading"><h2>Create a fresh Zigbee network</h2><button type="button" className="icon-button" onClick={() => setDialog(null)} aria-label="Close">✕</button></div><p>This deletes the current Zigbee network. All paired devices must be joined again.</p><p>Type <strong>DELETE</strong> to continue.</p>{dialogError && <p className="error" role="alert">{dialogError}</p>}<label className="field">Confirmation<input autoComplete="off" value={confirmation} onChange={event => setConfirmation(event.target.value)} /></label><div className="dialog-actions"><button type="button" className="button" onClick={() => setDialog(null)}>Cancel</button><button className="button danger" disabled={confirmation !== 'DELETE' || busy}>Create fresh network</button></div></form></div>}
  </main>
}

createRoot(document.getElementById('root')!).render(<App />)

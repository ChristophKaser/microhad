import { useEffect, useState } from 'react'
import type { ExternalDevice } from './api'
import { errorMessage, form, jsonRequest, post } from './api'

export function ExternalDeviceDialog({ id, onClose, onSaved }: { id: string; onClose: () => void; onSaved: () => void }) {
  const [kind, setKind] = useState<'choose' | 'shelly' | 'fritz'>(id.startsWith('fritz:') ? 'fritz' : id ? 'shelly' : 'choose')
  if (id.startsWith('fritz:') || kind === 'fritz') return <FritzBoxDialog id={id.startsWith('fritz:') ? id.slice(6) : ''} onClose={onClose} onSaved={onSaved} />
  if (id || kind === 'shelly') return <ShellyDialog id={id} onClose={onClose} onSaved={onSaved} />
  return <div className="modal"><section className="dialog" role="dialog" aria-modal="true" aria-label="Add external device">
    <div className="dialog-heading"><div><p className="eyebrow">Local HTTP device</p><h2>Add external device</h2></div><button className="icon-button" onClick={onClose} aria-label="Close">✕</button></div>
    <p>Select a device type to set up.</p><div className="dialog-actions"><button className="button primary" onClick={() => setKind('shelly')}>Shelly</button><button className="button primary" onClick={() => setKind('fritz')}>Fritz!Box</button><button className="button" onClick={onClose}>Close</button></div>
  </section></div>
}

interface FritzClient { mac: string; name: string; connected: boolean; has_signal?: boolean; signal_percent?: number }
interface FritzSaved { id: string; name: string; host: string; username: string; clients: { id: string; mac: string; name: string }[] }

function FritzBoxDialog({ id, onClose, onSaved }: { id: string; onClose: () => void; onSaved: () => void }) {
  const [boxId, setBoxId] = useState('')
  const [name, setName] = useState('Fritz!Box')
  const [host, setHost] = useState('192.168.178.1')
  const [username, setUsername] = useState('')
  const [password, setPassword] = useState('')
  const [clients, setClients] = useState<FritzClient[]>([])
  const [selected, setSelected] = useState<string[]>([])
  const [error, setError] = useState('')
  const [busy, setBusy] = useState(false)
  useEffect(() => {
    if (!id) return
    let active = true
    jsonRequest<FritzSaved[]>('/api/external/fritz/config').then(boxes => {
      const box = boxes.find(item => item.id === id || item.clients.some(client => client.id === id))
      if (!active || !box) return
      setBoxId(box.id)
      setName(box.name); setHost(box.host); setUsername(box.username)
      const saved = box.clients.map(client => ({ mac: client.mac, name: client.name, connected: false }))
      setClients(saved); setSelected(saved.map(client => client.mac))
    }).catch(reason => { if (active) setError(errorMessage(reason)) })
    return () => { active = false }
  }, [id])
  const discover = async () => {
    setBusy(true); setError('')
    try {
      const result = await post<{ clients: FritzClient[] }>('/api/external/fritz/discover', form({ host, username, password }))
      setClients(result.clients); setSelected(result.clients.map(client => client.mac))
    } catch (reason) { setError(errorMessage(reason)) }
    finally { setBusy(false) }
  }
  const save = async () => {
    setBusy(true); setError('')
    try {
      await post('/api/external/fritz/config', form({ id: boxId, name, host, username, password, selected_macs: selected.join(',') }))
      onSaved(); onClose()
    } catch (reason) { setError(errorMessage(reason)) }
    finally { setBusy(false) }
  }
  const remove = async () => {
    try { await jsonRequest(`/api/external/fritz/${encodeURIComponent(boxId)}`, { method: 'DELETE' }); onSaved(); onClose() }
    catch (reason) { setError(errorMessage(reason)) }
  }
  return <div className="modal"><section className="dialog wide" role="dialog" aria-modal="true" aria-label="Fritz!Box setup">
    <div className="dialog-heading"><div><p className="eyebrow">External device</p><h2>Fritz!Box Wi‑Fi presence</h2></div><button className="icon-button" onClick={onClose} aria-label="Close">✕</button></div>
    {error && <p className="error" role="alert">{error}</p>}
    <label className="field"><span>Name</span><input value={name} onChange={event => setName(event.target.value)} /></label>
    <label className="field"><span>Fritz!Box host</span><input value={host} onChange={event => setHost(event.target.value)} placeholder="192.168.178.1" /></label>
    <label className="field"><span>TR‑064 user</span><input value={username} onChange={event => setUsername(event.target.value)} autoComplete="username" /></label>
    <label className="field"><span>Password</span><input type="password" value={password} onChange={event => setPassword(event.target.value)} autoComplete="new-password" /></label>
    {id && <p className="muted small">Leave password blank to retain the saved password.</p>}
    <div className="dialog-actions"><button className="button" disabled={busy || !host.trim()} onClick={() => void discover()}>{busy ? 'Working…' : 'Discover Wi‑Fi devices'}</button></div>
    {clients.length > 0 && <fieldset className="field"><legend>Wi‑Fi devices to track</legend>{clients.map(client => <label className="check" key={client.mac}><input type="checkbox" checked={selected.includes(client.mac)} onChange={() => setSelected(current => current.includes(client.mac) ? current.filter(mac => mac !== client.mac) : [...current, client.mac])} />{client.name} <span className="muted small">{client.mac}{client.connected ? ' · connected' : ' · offline'}{client.has_signal ? ` · ${client.signal_percent}% signal` : ''}</span></label>)}</fieldset>}
    <p className="muted small">Presence and Wi‑Fi signal strength refresh every 30 seconds. FRITZ!Box reports signal strength as a percentage, and only while connected.</p>
    <div className="dialog-actions"><button className="button primary" disabled={busy || !name.trim() || !selected.length} onClick={() => void save()}>{busy ? 'Saving…' : 'Save external device'}</button>{boxId && <button className="button danger" onClick={() => void remove()}>Remove</button>}<button className="button" onClick={onClose}>Close</button></div>
  </section></div>
}

const gatewayHost = () => window.location.protocol === 'http:' ? window.location.host : window.location.hostname
const indices = (value: string) => value ? value.split(',').map(Number).filter(Number.isInteger) : []
const toggle = (values: number[], value: number) => values.includes(value) ? values.filter(item => item !== value) : [...values, value].sort()

export function ShellyDialog({ id, onClose, onSaved }: { id: string; onClose: () => void; onSaved: () => void }) {
  const [editing, setEditing] = useState<ExternalDevice | null>(null)
  const [model, setModel] = useState<'uni' | 'plug' | 'plugs'>('uni')
  const [name, setName] = useState('')
  const [host, setHost] = useState('')
  const [callbackHost, setCallbackHost] = useState(gatewayHost)
  const [username, setUsername] = useState('')
  const [password, setPassword] = useState('')
  const [inputs, setInputs] = useState<number[]>([0])
  const [temperatures, setTemperatures] = useState<number[]>([0])
  const [relays, setRelays] = useState<number[]>([0])
  const [pollSeconds, setPollSeconds] = useState(30)
  const [invert, setInvert] = useState(false)
  const [callbackMode, setCallbackMode] = useState(false)
  const [clearAuth, setClearAuth] = useState(false)
  const [error, setError] = useState('')
  const [loading, setLoading] = useState(!!id)
  const [saving, setSaving] = useState(false)
  const [saved, setSaved] = useState(false)
  const fill = (device: ExternalDevice) => {
    setEditing(device); setModel(device.model); setName(device.name); setHost(device.host)
    setCallbackHost(device.callback_host || gatewayHost())
    setInputs(device.input_indices !== undefined ? indices(device.input_indices) : device.input_index >= 0 ? [device.input_index] : [])
    setTemperatures(device.temperature_indices !== undefined ? indices(device.temperature_indices) : device.temperature_index >= 0 ? [device.temperature_index] : [])
    setRelays(device.relay_indices !== undefined ? indices(device.relay_indices) : device.model === 'uni' ? [device.relay_index] : [])
    setPollSeconds(device.poll_seconds); setInvert(device.invert_contact); setCallbackMode(device.callback_mode)
    setClearAuth(false); setUsername(''); setPassword('')
  }
  const load = async (deviceId: string) => {
    const devices = await jsonRequest<ExternalDevice[]>('/api/shelly/config')
    const device = devices.find((item) => item.id === deviceId)
    if (!device) throw new Error('Shelly device is no longer configured')
    fill(device)
  }
  useEffect(() => {
    if (!id) return
    let active = true
    jsonRequest<ExternalDevice[]>('/api/shelly/config')
      .then((devices) => {
        if (!active) return
        const device = devices.find((item) => item.id === id)
        if (device) fill(device)
        else setError('Shelly device is no longer configured')
      })
      .catch((reason) => { if (active) setError(errorMessage(reason)) })
      .finally(() => { if (active) setLoading(false) })
    return () => { active = false }
  }, [id])
  const save = async () => {
    setSaving(true); setError('')
    try {
      const result = await post<{ id: string }>('/api/shelly/config', form({
        id: editing?.id ?? '', name, host, callback_host: callbackHost, model, username, password,
        input_indices: model === 'uni' ? inputs.join(',') : '', temperature_indices: model === 'uni' ? temperatures.join(',') : '',
        relay_indices: model === 'uni' ? relays.join(',') : '', poll_seconds: pollSeconds,
        invert_contact: String(invert), callback_mode: String(callbackMode), clear_auth: String(clearAuth),
      }))
      onSaved()
      if (callbackMode) { await load(result.id); setSaved(true) }
      else onClose()
    } catch (reason) { setError(errorMessage(reason)) }
    finally { setSaving(false) }
  }
  const remove = async () => {
    if (!editing || !window.confirm(`Remove ${editing.name}?`)) return
    try {
      await jsonRequest(`/api/shelly/${encodeURIComponent(editing.id)}`, { method: 'DELETE' })
      onSaved(); onClose()
    } catch (reason) { setError(errorMessage(reason)) }
  }
  return <div className="modal" role="presentation" onMouseDown={(event) => { if (event.target === event.currentTarget) onClose() }}>
    <section className="dialog wide" role="dialog" aria-modal="true" aria-label={editing ? 'Shelly connection setup' : 'Add Shelly'}>
      <div className="dialog-heading"><div><p className="eyebrow">Local HTTP device</p><h2>{editing ? 'Shelly connection setup' : 'Add Shelly'}</h2></div><button className="icon-button" onClick={onClose} aria-label="Close">✕</button></div>
      {loading ? <p className="muted">Loading…</p> : <>
        {error && <p className="error" role="alert">{error}</p>}
        <label className="field"><span>Device type</span><select value={model} onChange={(e) => setModel(e.target.value as typeof model)}><option value="uni">Shelly Uni Gen1</option><option value="plug">Shelly Plug Gen1</option><option value="plugs">Shelly Plug S Gen1</option></select></label>
        <label className="field"><span>Name</span><input value={name} onChange={(e) => setName(e.target.value)} maxLength={80} /></label>
        <label className="field"><span>Host or IPv4 address</span><input value={host} onChange={(e) => setHost(e.target.value)} placeholder="192.168.1.42" /></label>
        <label className="field"><span>HTTP username</span><input value={username} onChange={(e) => setUsername(e.target.value)} autoComplete="username" /></label>
        <label className="field"><span>HTTP password</span><input type="password" value={password} onChange={(e) => setPassword(e.target.value)} autoComplete="new-password" /></label>
        {editing && <p className="muted small">Leave both credentials blank to keep the saved HTTP Basic credentials.</p>}
        {editing?.http_auth && <label className="check"><input type="checkbox" checked={clearAuth} onChange={(e) => setClearAuth(e.target.checked)} /> Disable saved HTTP authentication</label>}
        {model === 'uni' && <>
          <fieldset className="field"><legend>Contact inputs</legend>{[0, 1].map((channel) => <label className="check" key={`input-${channel}`}><input type="checkbox" checked={inputs.includes(channel)} onChange={() => setInputs(toggle(inputs, channel))} />Input {channel + 1}</label>)}</fieldset>
          <fieldset className="field"><legend>Temperature probes</legend>{[0, 1, 2].map((channel) => <label className="check" key={`temperature-${channel}`}><input type="checkbox" checked={temperatures.includes(channel)} onChange={() => setTemperatures(toggle(temperatures, channel))} />Probe {channel + 1}</label>)}</fieldset>
          <fieldset className="field"><legend>Relay actors</legend>{[0, 1].map((channel) => <label className="check" key={`relay-${channel}`}><input type="checkbox" checked={relays.includes(channel)} onChange={() => setRelays(toggle(relays, channel))} />Relay {channel + 1}</label>)}</fieldset>
          <label className="check"><input type="checkbox" checked={invert} onChange={(e) => setInvert(e.target.checked)} /> Invert contact state</label>
        </>}
        <label className="field"><span>Polling interval (seconds)</span><input type="number" min={5} max={3600} value={pollSeconds} onChange={(e) => setPollSeconds(Number(e.target.value))} /></label>
        <label className="check"><input type="checkbox" checked={callbackMode} onChange={(e) => setCallbackMode(e.target.checked)} /> Use HTTP callbacks for contact and relay state</label>
        {callbackMode && <><label className="field"><span>Gateway callback address</span><input value={callbackHost} onChange={(e) => setCallbackHost(e.target.value)} /></label><p className="muted small">Use a gateway HTTP address reachable by the Shelly. Temperature and Plug power continue to poll.</p></>}
        {editing?.callback_urls && <details open={saved}><summary>Callback URLs and setup</summary>
          <p className="muted small">Assign each URL to the matching Shelly action. Keep these token-bearing URLs private.</p>
          {Object.entries(editing.callback_urls).map(([event, url]) => <div key={event}><p className="small">{event.replace(/_/g, ' ')}</p><code>{url}</code></div>)}
        </details>}
        <div className="dialog-actions"><button className="button primary" onClick={() => void save()} disabled={saving || !name.trim() || !host.trim() || (callbackMode && !callbackHost.trim())}>{saving ? 'Saving…' : 'Save Shelly'}</button>{editing && <button className="button danger" onClick={() => void remove()}>Remove</button>}<button className="button" onClick={onClose}>Close</button></div>
      </>}
    </section>
  </div>
}

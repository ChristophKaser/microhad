import { useEffect, useState } from 'react'
import type { ExternalDevice } from './api'
import { errorMessage, form, jsonRequest, post } from './api'

type ConfigDevice = ExternalDevice

export function ShellyDevices({ devices, refresh }: { devices: ExternalDevice[]; refresh: () => void }) {
  const [configured, setConfigured] = useState<ConfigDevice[]>([])
  const [editing, setEditing] = useState<ConfigDevice | null>(null)
  const [model, setModel] = useState<'uni' | 'plug' | 'plugs'>('uni')
  const [name, setName] = useState('')
  const [host, setHost] = useState('')
  const [username, setUsername] = useState('')
  const [password, setPassword] = useState('')
  const [input, setInput] = useState(0)
  const [temperature, setTemperature] = useState(0)
  const [relay, setRelay] = useState(0)
  const [pollSeconds, setPollSeconds] = useState(30)
  const [invert, setInvert] = useState(false)
  const [callbackMode, setCallbackMode] = useState(false)
  const [clearAuth, setClearAuth] = useState(false)
  const [showForm, setShowForm] = useState(false)
  const [error, setError] = useState('')

  const load = async () => {
    try { setConfigured(await jsonRequest<ConfigDevice[]>('/api/shelly/config')) }
    catch (reason) { setError(errorMessage(reason)) }
  }
  useEffect(() => { void load() }, [])

  const reset = () => {
    setShowForm(true)
    setEditing(null); setModel('uni'); setName(''); setHost(''); setUsername(''); setPassword('')
    setInput(0); setTemperature(0); setRelay(0); setPollSeconds(30); setInvert(false); setCallbackMode(false); setClearAuth(false); setError('')
  }
  const edit = (device: ConfigDevice) => {
    setShowForm(true)
    setEditing(device); setModel(device.model); setName(device.name); setHost(device.host); setUsername(''); setPassword('')
    setInput(device.input_index); setTemperature(device.temperature_index); setRelay(device.relay_index)
    setPollSeconds(device.poll_seconds); setInvert(device.invert_contact); setCallbackMode(device.callback_mode); setClearAuth(false); setError('')
  }
  const save = async () => {
    try {
      await post('/api/shelly/config', form({
        id: editing?.id ?? '', name, host, model, username, password,
        input_index: model === 'uni' ? input : -1,
        temperature_index: model === 'uni' ? temperature : -1,
        relay_index: model === 'uni' ? relay : 0,
        poll_seconds: pollSeconds, invert_contact: String(invert), callback_mode: String(callbackMode), clear_auth: String(clearAuth),
      }))
      reset(); setShowForm(false); await load(); refresh()
    } catch (reason) { setError(errorMessage(reason)) }
  }
  const remove = async (device: ConfigDevice) => {
    if (!window.confirm(`Remove ${device.name}?`)) return
    try {
      const response = await fetch(`/api/shelly/${encodeURIComponent(device.id)}`, { method: 'DELETE' })
      const result = await response.json()
      if (!response.ok) throw new Error(result.error || 'Request failed')
      await load(); refresh()
    } catch (reason) { setError(errorMessage(reason)) }
  }
  const actor = async (device: ExternalDevice, state: 'on' | 'off') => {
    try { await post(`/api/shelly/${encodeURIComponent(device.id)}/actor`, form({ state })); refresh() }
    catch (reason) { setError(errorMessage(reason)) }
  }
  const liveById = new Map(devices.map((device) => [device.id, device]))

  return (
    <section className="panel shelly-panel">
      <div className="section-heading">
        <div><p className="eyebrow">Local HTTP devices</p><h2>Shelly devices</h2></div>
        <button className="button primary" onClick={reset}>Add Shelly</button>
      </div>
      {error && <p className="error" role="alert">{error}</p>}
      {showForm ? (
        <div className="shelly-form">
          <label className="field"><span>Device type</span><select value={model} onChange={(e) => setModel(e.target.value as typeof model)}>
            <option value="uni">Shelly Uni Gen1</option><option value="plug">Shelly Plug Gen1</option><option value="plugs">Shelly Plug S Gen1</option>
          </select></label>
          <label className="field"><span>Name</span><input value={name} onChange={(e) => setName(e.target.value)} maxLength={80} /></label>
          <label className="field"><span>Host or IPv4 address</span><input value={host} onChange={(e) => setHost(e.target.value)} placeholder="192.168.1.42" /></label>
          <label className="field"><span>HTTP username</span><input value={username} onChange={(e) => setUsername(e.target.value)} autoComplete="username" /></label>
          <label className="field"><span>HTTP password</span><input type="password" value={password} onChange={(e) => setPassword(e.target.value)} autoComplete="new-password" /></label>
          {editing && <p className="muted small">Leave both credential fields blank to keep the saved HTTP Basic credentials.</p>}
          {editing?.http_auth && <label className="check"><input type="checkbox" checked={clearAuth} onChange={(e) => setClearAuth(e.target.checked)} /> Disable saved HTTP authentication</label>}
          {model === 'uni' && <>
            <label className="field"><span>Contact input</span><select value={input} onChange={(e) => setInput(Number(e.target.value))}><option value={0}>Input 1</option><option value={1}>Input 2</option></select></label>
            <label className="field"><span>Temperature probe</span><select value={temperature} onChange={(e) => setTemperature(Number(e.target.value))}><option value={0}>Probe 1</option><option value={1}>Probe 2</option><option value={2}>Probe 3</option></select></label>
            <label className="field"><span>Relay actor</span><select value={relay} onChange={(e) => setRelay(Number(e.target.value))}><option value={0}>Relay 1</option><option value={1}>Relay 2</option></select></label>
            <label className="check"><input type="checkbox" checked={invert} onChange={(e) => setInvert(e.target.checked)} /> Invert contact state</label>
          </>}
          <label className="field"><span>Polling interval (seconds)</span><input type="number" min={5} max={3600} value={pollSeconds} onChange={(e) => setPollSeconds(Number(e.target.value))} /></label>
          <label className="check"><input type="checkbox" checked={callbackMode} onChange={(e) => setCallbackMode(e.target.checked)} /> Use HTTP callbacks for contact and relay state</label>
          {callbackMode && <p className="muted small">Temperature continues to poll. After saving, copy the callback URLs below into the Shelly action settings.</p>}
          <div className="toolbar"><button className="button primary" onClick={() => void save()} disabled={!name.trim() || !host.trim()}>Save Shelly</button><button className="button" onClick={() => { reset(); setShowForm(false) }}>Cancel</button></div>
        </div>
      ) : null}
      {!configured.length && !showForm && <div className="empty">No Shelly devices configured.</div>}
      <div className="device-grid">
        {configured.map((config) => {
          const device = liveById.get(config.id) ?? config
          return <article className="device-card" key={config.id}>
            <div className="device-head"><div className="grow"><h3>{config.name}</h3><span className="muted small">{config.model.toUpperCase()} · {config.host}{config.http_auth ? ' · HTTP Basic auth' : ''}</span></div></div>
            {device.has_contact && <p>Contact: <strong>{device.contact_known ? (device.contact_open ? 'Open' : 'Closed') : 'Unknown'}</strong></p>}
            {device.has_temperature && <p>Temperature: <strong>{device.temperature_known ? `${device.temperature_celsius} °C` : 'Unknown'}</strong></p>}
            {device.has_actor && <div className="toolbar"><span>Actor: <strong>{device.actor_known ? (device.actor_on ? 'On' : 'Off') : 'Unknown'}</strong></span><button className="button" onClick={() => void actor(device, device.actor_known && device.actor_on ? 'off' : 'on')}>{device.actor_known && device.actor_on ? 'Turn off' : 'Turn on'}</button></div>}
            <p className="muted small">{config.callback_mode ? 'Callbacks enabled for contact and relay; temperature polling enabled.' : `Polling every ${config.poll_seconds}s.`}</p>
            {config.callback_urls && <details><summary>Callback URLs and setup</summary>
              <p className="muted small">In Shelly Settings → Actions, assign each URL to the matching action and channel:</p>
              {config.model === 'uni' && <>
                <p className="small">Input {config.input_index + 1} · <strong>btn_on_url</strong></p><code>{config.callback_urls.input_open}</code>
                <p className="small">Input {config.input_index + 1} · <strong>btn_off_url</strong></p><code>{config.callback_urls.input_closed}</code>
              </>}
              <p className="small">Relay {config.model === 'uni' ? config.relay_index + 1 : 1} · <strong>out_on_url</strong></p><code>{config.callback_urls.relay_on}</code>
              <p className="small">Relay {config.model === 'uni' ? config.relay_index + 1 : 1} · <strong>out_off_url</strong></p><code>{config.callback_urls.relay_off}</code>
            </details>}
            <div className="toolbar"><button className="button" onClick={() => edit(config)}>Edit</button><button className="button danger" onClick={() => void remove(config)}>Remove</button></div>
          </article>
        })}
      </div>
    </section>
  )
}

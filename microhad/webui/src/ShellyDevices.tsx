import { useEffect, useState } from 'react'
import type { ExternalDevice } from './api'
import { errorMessage, form, jsonRequest, post } from './api'

const gatewayHost = () => window.location.protocol === 'http:' ? window.location.host : window.location.hostname

export function ShellyDialog({ id, onClose, onSaved }: { id: string; onClose: () => void; onSaved: () => void }) {
  const [editing, setEditing] = useState<ExternalDevice | null>(null)
  const [model, setModel] = useState<'uni' | 'plug' | 'plugs'>('uni')
  const [name, setName] = useState('')
  const [host, setHost] = useState('')
  const [callbackHost, setCallbackHost] = useState(gatewayHost)
  const [username, setUsername] = useState('')
  const [password, setPassword] = useState('')
  const [input, setInput] = useState(0)
  const [temperature, setTemperature] = useState(0)
  const [relay, setRelay] = useState(0)
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
    setInput(device.input_index); setTemperature(device.temperature_index); setRelay(device.relay_index)
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
        input_index: model === 'uni' ? input : -1, temperature_index: model === 'uni' ? temperature : -1,
        relay_index: model === 'uni' ? relay : 0, poll_seconds: pollSeconds,
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
          <label className="field"><span>Contact input</span><select value={input} onChange={(e) => setInput(Number(e.target.value))}><option value={0}>Input 1</option><option value={1}>Input 2</option></select></label>
          <label className="field"><span>Temperature probe</span><select value={temperature} onChange={(e) => setTemperature(Number(e.target.value))}><option value={-1}>None</option><option value={0}>Probe 1</option><option value={1}>Probe 2</option><option value={2}>Probe 3</option></select></label>
          <label className="field"><span>Relay actor</span><select value={relay} onChange={(e) => setRelay(Number(e.target.value))}><option value={0}>Relay 1</option><option value={1}>Relay 2</option></select></label>
          <label className="check"><input type="checkbox" checked={invert} onChange={(e) => setInvert(e.target.checked)} /> Invert contact state</label>
        </>}
        <label className="field"><span>Polling interval (seconds)</span><input type="number" min={5} max={3600} value={pollSeconds} onChange={(e) => setPollSeconds(Number(e.target.value))} /></label>
        <label className="check"><input type="checkbox" checked={callbackMode} onChange={(e) => setCallbackMode(e.target.checked)} /> Use HTTP callbacks for contact and relay state</label>
        {callbackMode && <><label className="field"><span>Gateway callback address</span><input value={callbackHost} onChange={(e) => setCallbackHost(e.target.value)} /></label><p className="muted small">Use a gateway HTTP address reachable by the Shelly. Temperature and Plug power continue to poll.</p></>}
        {editing?.callback_urls && <details open={saved}><summary>Callback URLs and setup</summary>
          <p className="muted small">Assign each URL to the matching Shelly action. Keep these token-bearing URLs private.</p>
          {model === 'uni' && <><p className="small">Input {input + 1} · btn_on_url</p><code>{editing.callback_urls.input_open}</code><p className="small">Input {input + 1} · btn_off_url</p><code>{editing.callback_urls.input_closed}</code></>}
          <p className="small">Relay {model === 'uni' ? relay + 1 : 1} · out_on_url</p><code>{editing.callback_urls.relay_on}</code>
          <p className="small">Relay {model === 'uni' ? relay + 1 : 1} · out_off_url</p><code>{editing.callback_urls.relay_off}</code>
        </details>}
        <div className="dialog-actions"><button className="button primary" onClick={() => void save()} disabled={saving || !name.trim() || !host.trim() || (callbackMode && !callbackHost.trim())}>{saving ? 'Saving…' : 'Save Shelly'}</button>{editing && <button className="button danger" onClick={() => void remove()}>Remove</button>}<button className="button" onClick={onClose}>Close</button></div>
      </>}
    </section>
  </div>
}

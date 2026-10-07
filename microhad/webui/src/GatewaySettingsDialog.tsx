import { useEffect, useRef, useState } from 'react'
import { errorMessage, form, jsonRequest, post } from './api'

interface GatewaySettings {
  broadcast_passive_ack_quorum: number
  applied_value: number | null
  application_state: 'pending' | 'applied' | 'error' | 'unsupported'
  application_error: string
}

export function GatewaySettingsDialog({ onClose }: { onClose: () => void }) {
  const [settings, setSettings] = useState<GatewaySettings | null>(null)
  const [value, setValue] = useState('')
  const [saving, setSaving] = useState(false)
  const [error, setError] = useState('')
  const initialized = useRef(false)

  useEffect(() => {
    let active = true
    let timer: ReturnType<typeof setTimeout> | undefined
    async function refresh() {
      try {
        const data = await jsonRequest<GatewaySettings>('/api/settings')
        if (!active) return
        setSettings(data)
        if (!initialized.current) {
          setValue(String(data.broadcast_passive_ack_quorum))
          initialized.current = true
        }
      } catch (reason) {
        if (active) setError(errorMessage(reason))
      }
      if (active) timer = setTimeout(() => void refresh(), 1000)
    }
    void refresh()
    return () => { active = false; if (timer) clearTimeout(timer) }
  }, [])

  async function save(event: React.FormEvent) {
    event.preventDefault()
    if (!/^\d{1,3}$/.test(value) || Number(value) < 1 || Number(value) > 255) {
      setError('Enter an integer between 1 and 255.')
      return
    }
    setSaving(true)
    setError('')
    try {
      setSettings(await post<GatewaySettings>('/api/settings', form({ broadcast_passive_ack_quorum: Number(value) })))
    } catch (reason) {
      setError(errorMessage(reason))
    } finally {
      setSaving(false)
    }
  }

  const unsupported = settings?.application_state === 'unsupported'
  return <div className="modal">
    <form className="dialog" onSubmit={event => void save(event)} aria-labelledby="gateway-settings-title">
      <div className="dialog-heading">
        <h2 id="gateway-settings-title">Gateway settings</h2>
        <button type="button" className="icon-button" onClick={onClose} aria-label="Close">✕</button>
      </div>
      {error && <p className="error" role="alert">{error}</p>}
      <label className="field">
        <span>Broadcast confirmation threshold</span>
        <input type="number" min="1" max="255" step="1" required value={value}
          disabled={!settings || saving || unsupported} onChange={event => setValue(event.target.value)} />
      </label>
      <p className="muted">Number of eligible neighboring routers whose rebroadcasts confirm delivery.
        If fewer routers are eligible, all are required. Default: 8.</p>
      <p className="muted">Changes are saved and applied immediately. They are restored after a service restart.</p>
      <div role="status" aria-live="polite">
        {!settings ? <p>Loading settings…</p> : <>
          <p>Saved: {settings.broadcast_passive_ack_quorum} · Applied: {settings.applied_value ?? 'Unknown'}</p>
          {settings.application_state === 'pending' && <p>Saved; awaiting Ziggurat. If disconnected, this will apply after reconnect.</p>}
          {settings.application_state === 'applied' && <p>Setting applied.</p>}
          {settings.application_state === 'error' && <p className="error">{settings.application_error}</p>}
          {unsupported && <p>This setting is unavailable with the current coordinator.</p>}
        </>}
      </div>
      <div className="dialog-actions">
        <button type="button" className="secondary" onClick={onClose}>Cancel</button>
        <button type="submit" disabled={!settings || saving || unsupported}>{saving ? 'Saving…' : 'Save settings'}</button>
      </div>
    </form>
  </div>
}

import { useEffect, useState } from 'react'
import { errorMessage, form, jsonRequest, post } from './api'

interface PushClient {
  id: string
  model: string
  label: string
  enabled: boolean
  kind: 'phone' | 'browser'
}

export function PushDevices({ enabled, onClose, onEnableBrowser, onTest, onToggle }: {
  enabled: boolean
  onClose: () => void
  onEnableBrowser: () => Promise<void>
  onTest: () => void
  onToggle: (enabled: boolean) => void
}) {
  const [clients, setClients] = useState<PushClient[]>([])
  const [error, setError] = useState('')
  const [busy, setBusy] = useState<string | null>(null)
  const refresh = async () => {
    try {
      const [phones, browsers] = await Promise.all([
        jsonRequest<{ devices: Omit<PushClient, 'kind'>[] }>('/api/push/devices'),
        jsonRequest<{ clients: Omit<PushClient, 'kind'>[] }>('/api/push/browser-clients'),
      ])
      setClients([
        ...phones.devices.map((client) => ({ ...client, kind: 'phone' as const })),
        ...browsers.clients.map((client) => ({ ...client, kind: 'browser' as const })),
      ])
      setError('')
    } catch (failure) { setError(errorMessage(failure)) }
  }
  useEffect(() => { void refresh() }, [])
  const update = async (client: PushClient, changes: Partial<Pick<PushClient, 'label' | 'enabled'>>) => {
    const updated = { ...client, ...changes }
    setBusy(client.id)
    try {
      const base = client.kind === 'phone' ? '/api/push/devices/' : '/api/push/browser-clients/'
      await post(`${base}${encodeURIComponent(client.id)}`, form({ label: updated.label, enabled: String(updated.enabled) }))
      await refresh()
    } catch (failure) { setError(errorMessage(failure)) }
    finally { setBusy(null) }
  }
  const remove = async (client: PushClient) => {
    if (!window.confirm(`Remove ${client.label || client.model}?`)) return
    setBusy(client.id)
    try {
      const base = client.kind === 'phone' ? '/api/push/devices/' : '/api/push/browser-clients/'
      await post(`${base}${encodeURIComponent(client.id)}/delete`)
      await refresh()
    } catch (failure) { setError(errorMessage(failure)) }
    finally { setBusy(null) }
  }
  return <div className="modal" onMouseDown={(event) => { if (event.target === event.currentTarget) onClose() }}>
    <section className="dialog wide push-dialog" role="dialog" aria-modal="true" aria-labelledby="push-dialog-title">
      <div className="dialog-heading">
        <div><p className="eyebrow">Notifications</p><h2 id="push-dialog-title">Push clients</h2></div>
        <button className="icon-button" onClick={onClose} aria-label="Close">✕</button>
      </div>
      <div className="push-toolbar">
        <label className="check"><input type="checkbox" checked={enabled} onChange={(event) => onToggle(event.target.checked)} /> Push notifications</label>
        <button className="button" onClick={() => { void onEnableBrowser().then(refresh) }}>Add browser</button>
        <button className="button" disabled={!enabled} onClick={onTest}>Test notification</button>
        <button className="button" onClick={() => void refresh()}>Refresh</button>
      </div>
      {error && <p role="alert" className="state-bad">{error}</p>}
      {!clients.length && !error && <p className="muted">No push clients have registered yet.</p>}
      <div className="push-phones">
        {clients.map((client) => <form key={`${client.kind}:${client.id}`} className="push-phone" onSubmit={(event) => { event.preventDefault(); void update(client, {}) }}>
          <div><strong>{client.label || client.model}</strong><p className="muted small">{client.kind === 'browser' ? 'Browser' : client.model} · {client.id}</p></div>
          <label>Label<input aria-label={`Label for ${client.label || client.model}`} maxLength={64} value={client.label}
            onChange={(event) => setClients((current) => current.map((item) => item.id === client.id && item.kind === client.kind ? { ...item, label: event.target.value } : item))} /></label>
          <label className="check"><input type="checkbox" checked={client.enabled} disabled={busy === client.id}
            onChange={(event) => void update(client, { enabled: event.target.checked })} /> Enabled</label>
          <button className="button" type="submit" disabled={busy === client.id}>Save</button>
          <button className="button danger" type="button" disabled={busy === client.id} onClick={() => void remove(client)}>Remove</button>
        </form>)}
      </div>
      {import.meta.env.DEV && <p className="muted small">Browser push enrollment requires the gateway HTTPS dashboard.</p>}
    </section>
  </div>
}

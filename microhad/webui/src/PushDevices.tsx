import { useEffect, useState } from 'react'
import { errorMessage, form, jsonRequest, post } from './api'

interface Phone {
  id: string
  model: string
  label: string
  enabled: boolean
}

export function PushDevices() {
  const [phones, setPhones] = useState<Phone[]>([])
  const [error, setError] = useState('')
  const [busy, setBusy] = useState<string | null>(null)
  const refresh = async () => {
    try {
      const result = await jsonRequest<{ devices: Phone[] }>('/api/push/devices')
      setPhones(result.devices)
      setError('')
    } catch (failure) {
      setError(errorMessage(failure))
    }
  }
  useEffect(() => { void refresh() }, [])
  const update = async (phone: Phone) => {
    setBusy(phone.id)
    try {
      await post(`/api/push/devices/${encodeURIComponent(phone.id)}`, form({
        label: phone.label,
        enabled: String(phone.enabled),
      }))
      await refresh()
    } catch (failure) {
      setError(errorMessage(failure))
    } finally {
      setBusy(null)
    }
  }
  return (
    <section className="panel">
      <div className="section-heading">
        <div>
          <p className="eyebrow">Push notifications</p>
          <h2>Phones</h2>
        </div>
        <button className="button" onClick={() => void refresh()}>Refresh</button>
      </div>
      {error && <p role="alert" className="state-bad">{error}</p>}
      {!phones.length && !error && <p className="muted">No Pommern phones have registered yet. Open the app on your home Wi-Fi.</p>}
      <div className="push-phones">
        {phones.map((phone) => (
          <form key={phone.id} className="push-phone" onSubmit={(event) => { event.preventDefault(); void update(phone) }}>
            <div>
              <strong>{phone.label || phone.model}</strong>
              <p className="muted small">{phone.model} · {phone.id}</p>
            </div>
            <label>
              Label
              <input aria-label={`Label for ${phone.model}`} maxLength={64} value={phone.label}
                onChange={(event) => setPhones((current) => current.map((item) => item.id === phone.id ? { ...item, label: event.target.value } : item))} />
            </label>
            <label className="check">
              <input type="checkbox" checked={phone.enabled} disabled={busy === phone.id}
                onChange={(event) => void update({ ...phone, enabled: event.target.checked })} />
              Enabled
            </label>
            <button className="button" type="submit" disabled={busy === phone.id}>Save</button>
          </form>
        ))}
      </div>
    </section>
  )
}

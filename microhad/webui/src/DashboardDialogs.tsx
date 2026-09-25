import { useEffect, useState } from 'react'
import type { FormEvent } from 'react'
import type { Device } from './api'
import { errorMessage, form, post, textRequest } from './api'
import { ClipboardButton, DescriptorList } from './devices'

type DialogCallbacks = { onClose: () => void; onSaved: () => void }

export function DeviceDetailsDialog({ device, onClose }: { device: Device; onClose: () => void }) {
  const details = [
    `IEEE / EUI-64: ${device.ieee_address}`,
    device.manufacturer_name && `Vendor: ${device.manufacturer_name}`,
    device.model_id && `Model: ${device.model_id}`,
  ]
    .filter(Boolean)
    .join('\n')

  return (
    <div className="modal">
      <section
        className="dialog wide"
        role="dialog"
        aria-modal="true"
        aria-labelledby="device-details-title"
      >
        <div className="dialog-heading">
          <div>
            <p className="eyebrow">Device details</p>
            <h2 id="device-details-title">{device.name}</h2>
          </div>
          <button className="icon-button" onClick={onClose} aria-label="Close">
            ✕
          </button>
        </div>
        <dl className="device-identity">
          <div><dt>Address</dt><dd className="mono">{device.ieee_address}</dd></div>
          <div><dt>Type</dt><dd>{device.kind}</dd></div>
          <div><dt>Manufacturer</dt><dd>{device.manufacturer_name || 'Unknown'}</dd></div>
          <div><dt>Model</dt><dd>{device.model_id || 'Unknown'}</dd></div>
          <div><dt>Battery</dt><dd>{device.has_battery ? `${device.battery_percent}%` : 'Not reported'}</dd></div>
          <div><dt>Link quality</dt><dd>{device.link_quality}</dd></div>
          <div><dt>Last contact</dt><dd>{device.last_seen ? new Date(device.last_seen * 1000).toLocaleString() : 'Never'}</dd></div>
        </dl>
        <h3>Endpoint descriptors</h3>
        {device.endpoint_descriptors.length ? (
          device.endpoint_descriptors.map((item) => (
            <DescriptorList key={item.endpoint} descriptor={item} />
          ))
        ) : (
          <p className="muted">No endpoint descriptors have been saved for this device yet.</p>
        )}
        <div className="dialog-actions">
          <ClipboardButton text={details} />
          <button className="button" onClick={onClose}>
            Close
          </button>
        </div>
      </section>
    </div>
  )
}

export function SceneTextDialog({ onClose, onSaved }: DialogCallbacks) {
  const [text, setText] = useState('')
  const [loading, setLoading] = useState(true)
  const [loadFailed, setLoadFailed] = useState(false)
  const [saving, setSaving] = useState(false)
  const [error, setError] = useState('')

  useEffect(() => {
    let active = true
    textRequest('/api/scenes/config')
      .then((config) => {
        if (active) setText(config)
      })
      .catch((cause) => {
        if (active) {
          setError(errorMessage(cause))
          setLoadFailed(true)
        }
      })
      .finally(() => {
        if (active) setLoading(false)
      })
    return () => {
      active = false
    }
  }, [])

  const save = async () => {
    setSaving(true)
    setError('')
    try {
      await post('/api/scenes/config', text)
      onSaved()
      onClose()
    } catch (cause) {
      setError(errorMessage(cause))
    } finally {
      setSaving(false)
    }
  }

  return (
    <div className="modal">
      <section className="dialog wide" role="dialog" aria-modal="true" aria-labelledby="text-title">
        <div className="dialog-heading">
          <h2 id="text-title">Advanced scene text editor</h2>
          <button className="icon-button" onClick={onClose} aria-label="Close">
            ✕
          </button>
        </div>
        <p className="muted">Saving here replaces all scene and group settings.</p>
        {error && (
          <p className="error" role="alert">
            {error}
          </p>
        )}
        <textarea
          className="advanced-text"
          aria-label="Scene configuration"
          spellCheck={false}
          disabled={loading || loadFailed}
          value={text}
          onChange={(event) => setText(event.target.value)}
        />
        <div className="dialog-actions">
          <button className="button" onClick={onClose}>
            Cancel
          </button>
          <button className="button primary" disabled={loading || loadFailed || saving} onClick={save}>
            {saving ? 'Saving…' : 'Save & sync scenes'}
          </button>
        </div>
      </section>
    </div>
  )
}

export function NetworkDialog({ onClose, onSaved }: DialogCallbacks) {
  const [confirmation, setConfirmation] = useState('')
  const [saving, setSaving] = useState(false)
  const [error, setError] = useState('')

  const createNetwork = async (event: FormEvent) => {
    event.preventDefault()
    if (confirmation !== 'DELETE' || saving) return
    setSaving(true)
    setError('')
    try {
      await post('/api/network/create', form({ confirmation }))
      onSaved()
      onClose()
    } catch (cause) {
      setError(errorMessage(cause))
    } finally {
      setSaving(false)
    }
  }

  return (
    <div className="modal">
      <form className="dialog" onSubmit={createNetwork}>
        <div className="dialog-heading">
          <h2>Create a fresh Zigbee network</h2>
          <button type="button" className="icon-button" onClick={onClose} aria-label="Close">
            ✕
          </button>
        </div>
        <p>This deletes the current Zigbee network. All paired devices must be joined again.</p>
        <p>
          Type <strong>DELETE</strong> to continue.
        </p>
        {error && (
          <p className="error" role="alert">
            {error}
          </p>
        )}
        <label className="field">
          Confirmation
          <input
            autoComplete="off"
            value={confirmation}
            onChange={(event) => setConfirmation(event.target.value)}
          />
        </label>
        <div className="dialog-actions">
          <button type="button" className="button" onClick={onClose}>
            Cancel
          </button>
          <button className="button danger" disabled={confirmation !== 'DELETE' || saving}>
            Create fresh network
          </button>
        </div>
      </form>
    </div>
  )
}

import { useEffect, useState } from 'react'
import type { FormEvent } from 'react'
import type { Device, DeviceConfiguration } from './api'
import { errorMessage, form, post, textRequest } from './api'
import { ClipboardButton, DescriptorList } from './devices'

type DialogCallbacks = { onClose: () => void; onSaved: () => void }

export function DeviceDetailsDialog({ device, configuration, onClose }: {
  device: Device
  configuration?: DeviceConfiguration
  onClose: () => void
}) {
  const [identifying, setIdentifying] = useState(false)
  const [identifyResult, setIdentifyResult] = useState('')
  const [identifyError, setIdentifyError] = useState('')
  const [querying, setQuerying] = useState(false)
  const [queryError, setQueryError] = useState('')
  const [queryResult, setQueryResult] = useState('')
  const [reconfiguring, setReconfiguring] = useState(false)
  const [reconfigureResult, setReconfigureResult] = useState('')
  const [reconfigureError, setReconfigureError] = useState('')
  const canIdentify = device.endpoint_descriptors.some((endpoint) => endpoint.input_clusters.includes(0x0003))
  const identify = async () => {
    if (identifying) return
    setIdentifying(true)
    setIdentifyResult('')
    setIdentifyError('')
    try {
      await post(`/api/devices/${encodeURIComponent(device.id)}/identify`)
      setIdentifyResult('Identify command queued for 5 seconds.')
    } catch (cause) {
      setIdentifyError(errorMessage(cause))
    } finally {
      setIdentifying(false)
    }
  }
  const queryConfiguration = async () => {
    if (querying) return
    setQuerying(true)
    setQueryError('')
    setQueryResult('')
    try {
      const result = await post<{ message: string }>(
        `/api/devices/${encodeURIComponent(device.id)}/configuration`,
      )
      setQueryResult(result.message)
    } catch (cause) {
      setQueryError(errorMessage(cause))
    } finally {
      setQuerying(false)
    }
  }
  const reconfigure = async () => {
    if (reconfiguring) return
    setReconfiguring(true)
    setReconfigureResult('')
    setReconfigureError('')
    try {
      const result = await post<{ message: string }>(
        `/api/devices/${encodeURIComponent(device.id)}/reconfigure`,
      )
      setReconfigureResult(`${result.message} Battery devices may need to wake to finish setup.`)
    } catch (cause) {
      setReconfigureError(errorMessage(cause))
    } finally {
      setReconfiguring(false)
    }
  }
  const details = [
    `IEEE / EUI-64: ${device.ieee_address}`,
    device.manufacturer_name && `Vendor: ${device.manufacturer_name}`,
    device.model_id && `Model: ${device.model_id}`,
  ]
    .filter(Boolean)
    .join('\n')

  if (device.source === 'shelly' || device.source === 'fritz_box') return <div className="modal"><section className="dialog" role="dialog" aria-modal="true" aria-label={`Details for ${device.name}`}>
    <div className="dialog-heading"><h2>{device.name}</h2><button className="icon-button" onClick={onClose} aria-label="Close">✕</button></div>
    <dl className="device-identity"><div><dt>Type</dt><dd>{device.kind}</dd></div><div><dt>Source</dt><dd>{device.source === 'fritz_box' ? 'Fritz!Box Wi‑Fi' : 'Shelly HTTP'}</dd></div><div><dt>Status</dt><dd>{device.source === 'fritz_box' ? (!device.last_seen || Date.now() / 1000 - device.last_seen > 90 ? 'Unknown' : device.occupied ? 'Present' : 'Not present') : device.model_id}</dd></div>{device.source === 'fritz_box' && <div><dt>Wi‑Fi signal</dt><dd>{device.has_wifi_signal ? `${device.wifi_signal_percent}%` : 'Unavailable'}</dd></div>}<div><dt>Last contact</dt><dd>{device.last_seen ? new Date(device.last_seen * 1000).toLocaleString() : 'Never'}</dd></div></dl>
    <div className="dialog-actions"><button className="button" onClick={onClose}>Close</button></div>
  </section></div>

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
          <div><dt>{device.source === 'matter' ? 'Direct Thread RSSI' : 'Link quality'}</dt><dd>{device.source === 'matter' ? (device.matter_rssi_dbm === null || device.matter_rssi_dbm === undefined ? 'Unavailable' : `${device.matter_rssi_dbm} dBm`) : device.link_quality}</dd></div>
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
        <h3>Device configuration</h3>
        <p className="muted">Read the node descriptor, Groups membership, and the device’s Zigbee binding table. Battery devices may need to wake before replying.</p>
        <div className="editor-card">
          <p><strong>Node descriptor:</strong> {configuration?.node_descriptor_status || (device.node_descriptor ? 'stored' : 'Not queried')}</p>
          {device.node_descriptor && (() => {
            const descriptor = device.node_descriptor
            const logicalTypes = ['Coordinator', 'Router', 'End device', 'Reserved']
            const frequencyBands = [
              descriptor.frequency_band & 0x01 ? '868 MHz' : '',
              descriptor.frequency_band & 0x02 ? '902–928 MHz' : '',
              descriptor.frequency_band & 0x04 ? '2.4 GHz' : '',
            ].filter(Boolean)
            return <>
              <dl className="device-identity">
                <div><dt>Logical type</dt><dd>{logicalTypes[descriptor.logical_device_type] || 'Unknown'}</dd></div>
                <div><dt>Frequency bands</dt><dd>{frequencyBands.length ? frequencyBands.join(', ') : `Unknown (0x${descriptor.frequency_band.toString(16)})`}</dd></div>
                <div><dt>APS flags</dt><dd>0x{descriptor.aps_flags.toString(16).padStart(2, '0')}</dd></div>
                <div><dt>Manufacturer code</dt><dd>0x{descriptor.manufacturer_code.toString(16).padStart(4, '0')}</dd></div>
                <div><dt>Maximum buffer</dt><dd>{descriptor.max_buffer_size} bytes</dd></div>
                <div><dt>Maximum incoming transfer</dt><dd>{descriptor.max_incoming_transfer_size} bytes</dd></div>
                <div><dt>Maximum outgoing transfer</dt><dd>{descriptor.max_outgoing_transfer_size} bytes</dd></div>
                <div><dt>Server mask</dt><dd>0x{descriptor.server_mask.toString(16).padStart(4, '0')}</dd></div>
                <div><dt>Descriptor capability</dt><dd>0x{descriptor.descriptor_capability.toString(16).padStart(2, '0')}</dd></div>
              </dl>
              <p><strong>MAC capabilities:</strong> {[descriptor.alternate_pan_coordinator && 'Alternate coordinator', descriptor.full_function_device && 'Full function device', descriptor.mains_powered && 'Mains powered', descriptor.receiver_on_when_idle && 'Receiver on when idle', descriptor.security_capability && 'Security capable', descriptor.allocate_address && 'Can allocate addresses'].filter(Boolean).join(', ') || 'None reported'}</p>
              <p><strong>Optional descriptors:</strong> {[
                descriptor.complex_descriptor_available && 'Complex descriptor available',
                descriptor.user_descriptor_available && 'User descriptor available',
              ].filter(Boolean).join(', ') || 'None reported'}</p>
              <p><strong>Raw descriptor:</strong> <code>{descriptor.raw_hex}</code></p>
            </>
          })()}
          <p><strong>Groups membership:</strong> {configuration?.groups_status || 'Not queried'}</p>
          {configuration?.groups_status === 'complete' && (
            <p>Group IDs: {configuration.groups.length ? configuration.groups.join(', ') : 'None'}</p>
          )}
          <p><strong>Binding table:</strong> {configuration?.bindings_status || 'Not queried'}</p>
          {configuration?.bindings_status === 'complete' && (
            configuration.bindings.length ? (
              <ul>
                {configuration.bindings.map((binding, index) => (
                  <li key={`${binding.cluster_id}-${index}`}>
                    Cluster 0x{binding.cluster_id.toString(16).padStart(4, '0')} → {binding.group_destination
                      ? `group ${binding.group_id}`
                      : `${binding.ieee_address}, endpoint ${binding.endpoint}`}
                  </li>
                ))}
              </ul>
            ) : <p>No bindings reported.</p>
          )}
          {queryError && <p className="error" role="alert">{queryError}</p>}
          {queryResult && <p role="status">{queryResult}</p>}
          <button className="button" disabled={querying} onClick={queryConfiguration}>
            {querying ? 'Querying…' : 'Read device configuration'}
          </button>
        </div>
        {device.source === 'zigbee' && (
          <div className="editor-card">
            <p>Reconfigure reporting and bindings after a reset or when device setup needs repair.</p>
            {reconfigureError && <p className="error" role="alert">{reconfigureError}</p>}
            {reconfigureResult && <p role="status">{reconfigureResult}</p>}
            <button className="button" disabled={reconfiguring} onClick={reconfigure}>
              {reconfiguring ? 'Queuing…' : 'Reconfigure device'}
            </button>
          </div>
        )}
        {identifyError && <p className="error" role="alert">{identifyError}</p>}
        {identifyResult && <p role="status">{identifyResult}</p>}
        <div className="dialog-actions">
          <ClipboardButton text={details} />
          {canIdentify && (
            <button className="button" disabled={identifying} onClick={identify}>
              {identifying ? 'Sending…' : 'Identify (5 s)'}
            </button>
          )}
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
      const trimmedNames = text.split('\n').map(line => {
        const fields = line.split('\t')
        const nameIndex = fields[0] === 'G' ? 2 : fields[0] === 'S' ? 3 : -1
        if (nameIndex >= 0 && fields[nameIndex] !== undefined) fields[nameIndex] = fields[nameIndex].trim()
        return fields.join('\t')
      }).join('\n')
      await post('/api/scenes/config', trimmedNames)
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

export function NetworkDialog({ onClose, onSaved, existingNetwork }: DialogCallbacks & { existingNetwork: boolean }) {
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
          <h2>{existingNetwork ? 'Replace Zigbee network' : 'Create Zigbee network'}</h2>
          <button type="button" className="icon-button" onClick={onClose} aria-label="Close">
            ✕
          </button>
        </div>
        <p>{existingNetwork ? 'This replaces the current Zigbee network. All paired Zigbee devices must be joined again.' : 'This creates a Zigbee network for pairing devices.'}</p>
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
            {existingNetwork ? 'Replace Zigbee network' : 'Create Zigbee network'}
          </button>
        </div>
      </form>
    </div>
  )
}

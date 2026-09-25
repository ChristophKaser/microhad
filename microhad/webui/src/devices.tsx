import { useEffect, useState } from 'react'
import type { Device, EndpointDescriptor, Status } from './api'
import { errorMessage, form, post } from './api'
import { isDimmable } from './scenes'

const profileNames: Record<number, string> = {
  260: 'Home Automation',
  49246: 'Zigbee Light Link',
  41440: 'Green Power',
}
const deviceNames: Record<number, string> = {
  0: 'On/Off Switch',
  256: 'On/Off Light',
  257: 'Dimmable Light',
  770: 'Temperature Sensor',
  775: 'Humidity Sensor',
  1026: 'IAS Zone',
}
const clusterNames: Record<number, string> = {
  0: 'Basic',
  1: 'Power Configuration',
  3: 'Identify',
  4: 'Groups',
  5: 'Scenes',
  6: 'On/Off',
  8: 'Level Control',
  25: 'Over-the-Air Upgrade',
  32: 'Poll Control',
  768: 'Color Control',
  1024: 'Illuminance Measurement',
  1026: 'Temperature Measurement',
  1027: 'Pressure Measurement',
  1029: 'Relative Humidity',
  1030: 'Occupancy Sensing',
  1280: 'IAS Zone',
  1794: 'Simple Metering',
  1795: 'Messaging',
  1796: 'Tunneling',
  2817: 'BACnet Protocol Tunnel',
  2819: 'Appliance Events and Alerts',
  2820: 'Electrical Measurement',
}
const namedId = (id: number, names: Record<number, string>) =>
  `0x${id.toString(16).padStart(4, '0').toUpperCase()}${names[id] ? ` — ${names[id]}` : ''}`
function DeviceVisual({ device, type }: { device: Device; type: string }) {
  if (type === 'Contact sensors') return (
    <svg viewBox="0 0 64 64" fill="none" stroke="currentColor" strokeWidth="3" strokeLinejoin="round" aria-hidden="true">
      <rect x="13" y="8" width="38" height="48" rx="2" />
      {device.contact_open ? (
        <><path d="M17 11 43 17v38L17 50Z" fill="currentColor" fillOpacity=".18" /><circle cx="37" cy="35" r="2" fill="currentColor" stroke="none" /></>
      ) : (
        <><rect x="18" y="12" width="28" height="40" rx="1" fill="currentColor" fillOpacity=".18" /><circle cx="40" cy="33" r="2" fill="currentColor" stroke="none" /></>
      )}
    </svg>
  )
  if (type === 'Lights') return (
    <svg viewBox="0 0 64 64" fill="none" stroke="currentColor" strokeWidth="3" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
      <path d="M21 40c0-5-7-9-7-18a18 18 0 0 1 36 0c0 9-7 13-7 18H21Z" fill="currentColor" fillOpacity={device.light_on ? '.3' : '.08'} />
      <path d="M24 46h16M26 52h12M29 57h6" />
      {device.light_on && <path d="M32 1v4M7 11l4 3M57 11l-4 3M3 27h5M61 27h-5" />}
    </svg>
  )
  if (type === 'Motion sensors') return <span aria-hidden="true">{device.occupied ? '◉' : '◎'}</span>
  return <span aria-hidden="true">{type === 'Remotes' ? '🎛️' : '📡'}</span>
}
const deviceTypes = [
  { name: 'Contact sensors', matches: (device: Device) => device.has_contact },
  {
    name: 'Climate sensors',
    matches: (device: Device) => device.has_temperature || device.has_humidity,
  },
  { name: 'Motion sensors', matches: (device: Device) => device.has_occupancy },
  { name: 'Lights', matches: (device: Device) => device.has_light },
  { name: 'Remotes', matches: (device: Device) => device.has_remote },
  { name: 'Other devices', matches: (_device: Device) => true },
]
const deviceType = (device: Device) =>
  deviceTypes.find((type) => type.matches(device))?.name ?? 'Other devices'
const byName = (left: Device, right: Device) =>
  left.name.localeCompare(right.name, undefined, { sensitivity: 'base', numeric: true }) ||
  left.ieee_address.localeCompare(right.ieee_address)
function relativeTime(epoch: number): string {
  if (!epoch) return 'Never'
  const seconds = Math.max(0, Math.floor(Date.now() / 1000 - epoch))
  if (seconds < 15) return 'just now'
  if (seconds < 60) return `${seconds} sec ago`
  if (seconds < 3600) return `${Math.floor(seconds / 60)} min ago`
  if (seconds < 86400) return `${Math.floor(seconds / 3600)} hr ago`
  return `${Math.floor(seconds / 86400)} days ago`
}
export function ClipboardButton({ text }: { text: string }) {
  const [copied, setCopied] = useState(false)
  const copy = async () => {
    try {
      if (navigator.clipboard?.writeText) await navigator.clipboard.writeText(text)
      else {
        const input = document.createElement('textarea')
        input.value = text
        document.body.appendChild(input)
        input.select()
        document.execCommand('copy')
        input.remove()
      }
      setCopied(true)
    } catch (e) {
      window.alert(errorMessage(e))
    }
  }
  return (
    <button className="button" onClick={copy}>
      {copied ? '✓ Copied' : '⧉ Copy device info'}
    </button>
  )
}
export function DescriptorList({ descriptor }: { descriptor: EndpointDescriptor }) {
  return (
    <section className="descriptor-row">
      <h3>Endpoint {descriptor.endpoint}</h3>
      <p className="mono muted">
        Profile {namedId(descriptor.profile_id, profileNames)} · Device{' '}
        {namedId(descriptor.device_id, deviceNames)} · Version {descriptor.device_version}
      </p>
      <div className="descriptor-columns">
        <div>
          <b>Server / input clusters</b>
          <ul>
            {descriptor.input_clusters.map((id) => (
              <li key={id}>{namedId(id, clusterNames)}</li>
            ))}
            {!descriptor.input_clusters.length && <li>None</li>}
          </ul>
        </div>
        <div>
          <b>Client / output clusters</b>
          <ul>
            {descriptor.output_clusters.map((id) => (
              <li key={id}>{namedId(id, clusterNames)}</li>
            ))}
            {!descriptor.output_clusters.length && <li>None</li>}
          </ul>
        </div>
      </div>
    </section>
  )
}
function DeviceCard({
  device,
  action,
  onSettings,
  onDetails,
  onHistory,
}: {
  device: Device
  action: (url: string, body?: string) => void
  onSettings: () => void
  onDetails: () => void
  onHistory: () => void
}) {
  const [brightness, setBrightness] = useState(
    device.has_brightness ? device.brightness_percent : 100
  )
  useEffect(
    () => setBrightness(device.has_brightness ? device.brightness_percent : 100),
    [device.brightness_percent, device.has_brightness]
  )
  const url = `/api/devices/${encodeURIComponent(device.ieee_address)}`
  const canDim = isDimmable(device)
  const type = deviceType(device)
  const extraReadings = [
    type !== 'Climate sensors' && device.has_temperature && `🌡 ${device.temperature_celsius} °C`,
    type !== 'Climate sensors' && device.has_humidity && `💧 ${device.humidity_percent}%`,
    type !== 'Contact sensors' && device.has_contact && (device.contact_open ? 'Open' : 'Closed'),
    type !== 'Motion sensors' && device.has_occupancy && (device.occupied ? 'Motion' : 'Clear'),
    type !== 'Lights' && device.has_light && (device.light_on ? 'Light on' : 'Light off'),
  ].filter(Boolean)
  return (
    <article className={`device-card device-card-${type.toLowerCase().replace(/[^a-z]+/g, '-')} ${
      (type === 'Contact sensors' && device.contact_open) ||
      (type === 'Motion sensors' && device.occupied) ||
      (type === 'Lights' && device.light_on)
        ? 'device-active'
        : ''
    }`}>
      <div className="device-head">
        <div className="grow">
          <h3>{device.name}</h3>
        </div>
      </div>
      {type === 'Climate sensors' ? (
        <div className="device-focus climate-focus">
          {device.has_temperature && (
            <div className="climate-reading">
              <span className="reading-label">Temperature</span>
              <strong>{device.temperature_celsius}<span className="reading-unit">°C</span></strong>
            </div>
          )}
          {device.has_humidity && (
            <div className="climate-reading">
              <span className="reading-label">Humidity</span>
              <strong>{device.humidity_percent}<span className="reading-unit">%</span></strong>
            </div>
          )}
        </div>
      ) : (
        <div className={`device-focus ${type === 'Lights' ? 'light-focus' : ''}`}>
          <span className="focus-icon"><DeviceVisual device={device} type={type} /></span>
          <div className="focus-copy">
            <strong>{type === 'Contact sensors' ? (device.contact_open ? 'Open' : 'Closed')
              : type === 'Motion sensors' ? (device.occupied ? 'Occupied' : 'Clear')
              : type === 'Lights' ? (device.light_on ? 'On' : 'Off')
              : type === 'Remotes' ? 'Remote' : device.kind}</strong>
            <span>{type === 'Contact sensors' ? 'Contact sensor'
              : type === 'Motion sensors' ? 'Motion sensor'
              : type === 'Lights' ? 'Light'
              : type === 'Remotes' ? 'Control' : 'Device'}</span>
          </div>
        </div>
      )}
      {!!extraReadings.length && (
        <div className="extra-readings">
          {extraReadings.map((reading, index) => <span key={index}>{reading}</span>)}
        </div>
      )}
      {device.has_light && (
        <div className="light-panel">
          <button
            className={`button light-toggle ${device.light_on ? 'primary' : ''}`}
            onClick={() => action(`${url}/light`, form({ state: device.light_on ? 'off' : 'on' }))}
          >
            {device.light_on ? 'Turn off' : 'Turn on'}
          </button>
          {canDim && (
            <>
              <div className="brightness-row">
                <label htmlFor={`brightness-${device.ieee_address}`}>Brightness</label>
                <span>{brightness}%</span>
              </div>
              <input
                id={`brightness-${device.ieee_address}`}
                type="range"
                min="0"
                max="100"
                value={brightness}
                aria-label={`Brightness for ${device.name}`}
                onChange={(event) => setBrightness(Number(event.target.value))}
                onPointerUp={(event) =>
                  action(`${url}/light`, form({ brightness: event.currentTarget.value }))
                }
                onKeyUp={(event) => {
                  if (['ArrowLeft', 'ArrowRight', 'Home', 'End', 'PageUp', 'PageDown', 'Enter'].includes(event.key))
                    action(`${url}/light`, form({ brightness: event.currentTarget.value }))
                }}
              />
              <div className="presets">
                {[25, 50, 75, 100].map((level) => (
                  <button className="button" key={level} onClick={() => action(`${url}/light`, form({ brightness: level }))}>
                    {level}%
                  </button>
                ))}
              </div>
            </>
          )}
        </div>
      )}
      <div className="device-bottom">
        <div className="device-status">
          {device.has_battery && <span title="Battery">🔋 {device.battery_percent}%</span>}
          <span title="Link quality">LQI {device.link_quality}</span>
          <span title={device.last_seen ? new Date(device.last_seen * 1000).toLocaleString() : undefined}>
            Seen {relativeTime(device.last_seen)}
          </span>
        </div>
        <div className="mini-actions">
          <button
            title="Device details"
            aria-label={`Details for ${device.name}`}
            onClick={onDetails}
          >
            ⓘ
          </button>
          <button title="Settings" aria-label={`Settings for ${device.name}`} onClick={onSettings}>
            ⚙
          </button>
          {(device.has_temperature || device.has_humidity || device.has_contact) && (
            <button title="History" aria-label={`History for ${device.name}`} onClick={onHistory}>
              ▥
            </button>
          )}
          <button
            title="Refresh attributes"
            aria-label={`Refresh ${device.name}`}
            onClick={() => action(`${url}/refresh`)}
          >
            ↻
          </button>
        </div>
      </div>
    </article>
  )
}
export function DeviceGroups({
  status,
  action,
  onSettings,
  onDetails,
  onHistory,
}: {
  status: Status | null
  action: (url: string, body?: string) => void
  onSettings: (device: Device) => void
  onDetails: (device: Device) => void
  onHistory: (device: Device) => void
}) {
  if (!status?.devices.length) return <div className="empty">No Zigbee devices discovered yet.</div>
  const sceneGroups = [...status.scene_groups].sort((left, right) =>
    left.name.localeCompare(right.name, undefined, { sensitivity: 'base', numeric: true })
  )
  const groupByLight = new Map<string, number>()
  for (const group of sceneGroups) {
    for (const address of group.members) {
      if (!groupByLight.has(address)) groupByLight.set(address, group.id)
    }
  }
  return (
    <div className="device-groups">
      {deviceTypes.map((type) => {
        const devices = status.devices
          .filter((device) => deviceType(device) === type.name)
          .sort(byName)
        if (!devices.length) return null
        const sections =
          type.name === 'Lights'
            ? [
                ...sceneGroups.map((group) => ({
                  id: String(group.id),
                  name: group.name,
                  devices: devices.filter(
                    (device) => groupByLight.get(device.ieee_address) === group.id
                  ),
                })),
                {
                  id: 'ungrouped',
                  name: 'Ungrouped',
                  devices: devices.filter((device) => !groupByLight.has(device.ieee_address)),
                },
              ].filter((section) => section.devices.length)
            : [{ id: type.name, name: '', devices }]
        return (
          <section className="device-type" key={type.name}>
            <div className="device-type-heading">
              <h3>{type.name}</h3>
              <span>{devices.length}</span>
            </div>
            {sections.map((section) => (
              <div className="device-subgroup" key={section.id}>
                {type.name === 'Lights' && (
                  <h4 className="device-subgroup-heading">{section.name}</h4>
                )}
                <div className="device-grid">
                  {section.devices.map((device) => (
                    <DeviceCard
                      key={device.ieee_address}
                      device={device}
                      action={action}
                      onSettings={() => onSettings(device)}
                      onDetails={() => onDetails(device)}
                      onHistory={() => onHistory(device)}
                    />
                  ))}
                </div>
              </div>
            ))}
          </section>
        )
      })}
    </div>
  )
}
export function DeviceSettings({
  device,
  onClose,
  onSaved,
}: {
  device: Device
  onClose: () => void
  onSaved: () => void
}) {
  const [name, setName] = useState(device.name)
  const [low, setLow] = useState(
    device.has_low_temperature_alert ? String(device.low_temperature_alert) : ''
  )
  const [high, setHigh] = useState(
    device.has_high_temperature_alert ? String(device.high_temperature_alert) : ''
  )
  const [initial, setInitial] = useState(String(device.contact_initial_alert_minutes))
  const [reminder, setReminder] = useState(String(device.contact_reminder_alert_minutes))
  const [refresh, setRefresh] = useState(String(device.sensor_refresh_interval_minutes))
  const [error, setError] = useState('')
  const [saving, setSaving] = useState(false)
  const save = async (event: React.FormEvent) => {
    event.preventDefault()
    setSaving(true)
    setError('')
    const fields: Record<string, string> = { name }
    if (device.has_temperature) {
      fields.low = low
      fields.high = high
    }
    if (device.has_contact) {
      fields.initial_minutes = initial
      fields.reminder_minutes = reminder
    }
    if (device.has_temperature || device.has_humidity) fields.sensor_refresh_minutes = refresh
    try {
      await post(`/api/devices/${encodeURIComponent(device.ieee_address)}/settings`, form(fields))
      onSaved()
      onClose()
    } catch (e) {
      setError(errorMessage(e))
    } finally {
      setSaving(false)
    }
  }
  return (
    <div className="modal">
      <form className="dialog" onSubmit={save}>
        <div className="dialog-heading">
          <h2>Device settings</h2>
          <button type="button" className="icon-button" onClick={onClose} aria-label="Close">
            ✕
          </button>
        </div>
        <p className="muted">
          {device.name} · {device.ieee_address}
        </p>
        {error && (
          <p className="error" role="alert">
            {error}
          </p>
        )}
        <label className="field">
          Name
          <input
            value={name}
            onChange={(event) => setName(event.target.value)}
            maxLength={64}
            required
          />
        </label>
        {device.has_contact && (
          <>
            <label className="field">
              First alert after (minutes)
              <input
                type="number"
                min="1"
                step="1"
                value={initial}
                onChange={(event) => setInitial(event.target.value)}
                required
              />
            </label>
            <label className="field">
              Reminder every (minutes)
              <input
                type="number"
                min="1"
                step="1"
                value={reminder}
                onChange={(event) => setReminder(event.target.value)}
                required
              />
            </label>
          </>
        )}
        {device.has_temperature && (
          <>
            <label className="field">
              Low threshold °C (blank disables)
              <input
                type="number"
                step=".1"
                value={low}
                onChange={(event) => setLow(event.target.value)}
              />
            </label>
            <label className="field">
              High threshold °C (blank disables)
              <input
                type="number"
                step=".1"
                value={high}
                onChange={(event) => setHigh(event.target.value)}
              />
            </label>
          </>
        )}
        {(device.has_temperature || device.has_humidity) && (
          <label className="field">
            Fallback refresh interval (minutes; 0 disables)
            <input
              type="number"
              min="0"
              step="1"
              value={refresh}
              onChange={(event) => setRefresh(event.target.value)}
              required
            />
          </label>
        )}
        <div className="dialog-actions">
          <button type="button" className="button" onClick={onClose}>
            Cancel
          </button>
          <button className="button primary" disabled={saving}>
            {saving ? 'Saving…' : 'Save settings'}
          </button>
        </div>
      </form>
    </div>
  )
}

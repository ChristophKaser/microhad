import { OtaUpdate } from './OtaUpdate'
import { useEffect, useState } from 'react'
import { ThermostatPanel } from './ThermostatPanel'
import type { TemperatureSource } from './api'
import type { Device, EndpointDescriptor, Status } from './api'
import { errorMessage, form, post } from './api'
import { isDimmable } from './scenes'
import { DeleteDeviceDialog } from './DeviceDeletion'
import { matterSyncTime, matterSubscriptions } from './matter'
import type { MatterSubscriptionHealth } from './matter'

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
  91: 'Air Quality',
  25: 'Over-the-Air Upgrade',
  32: 'Poll Control',
  768: 'Color Control',
  1024: 'Illuminance Measurement',
  1026: 'Temperature Measurement',
  1027: 'Pressure Measurement',
  1029: 'Relative Humidity',
  1037: 'CO2 Concentration Measurement',
  1066: 'PM2.5 Concentration Measurement',
  1030: 'Occupancy Sensing',
  1280: 'IAS Zone',
  1794: 'Simple Metering',
  1795: 'Messaging',
  1796: 'Tunneling',
  2817: 'BACnet Protocol Tunnel',
  2819: 'Appliance Events and Alerts',
  2820: 'Electrical Measurement',
}
// Matter cluster names from the SDK ZAP catalog mirrored in
// micro-thread-matter-daemon/dependencies/rs-matter/xtask/src/pics.rs (CLUSTER_NAMES).
// Keep this separate from Zigbee: cluster IDs and names differ between protocols.
const matterClusterNames: Record<number, string> = {
  0x0003: 'Identify',
  0x0004: 'Groups',
  0x0006: 'On/Off',
  0x0008: 'Level Control',
  0x001C: 'Pulse Width Modulation',
  0x001D: 'Descriptor',
  0x001E: 'Binding',
  0x001F: 'Access Control',
  0x0025: 'Actions',
  0x0028: 'Basic Information',
  0x0029: 'OTA Software Update Provider',
  0x002A: 'OTA Software Update Requestor',
  0x002B: 'Localization Configuration',
  0x002C: 'Time Format Localization',
  0x002D: 'Unit Localization',
  0x002E: 'Power Source Configuration',
  0x002F: 'Power Source',
  0x0030: 'General Commissioning',
  0x0031: 'Network Commissioning',
  0x0032: 'Diagnostic Logs',
  0x0033: 'General Diagnostics',
  0x0034: 'Software Diagnostics',
  0x0035: 'Thread Network Diagnostics',
  0x0036: 'Wi-Fi Network Diagnostics',
  0x0037: 'Ethernet Network Diagnostics',
  0x0038: 'Time Synchronization',
  0x0039: 'Bridged Device Basic Information',
  0x003B: 'Switch',
  0x003C: 'Administrator Commissioning',
  0x003E: 'Operational Credentials',
  0x003F: 'Group Key Management',
  0x0040: 'Fixed Label',
  0x0041: 'User Label',
  0x0042: 'Proxy Configuration',
  0x0043: 'Proxy Discovery',
  0x0044: 'Proxy Valid',
  0x0045: 'Boolean State',
  0x0046: 'ICD Management',
  0x0047: 'Timer',
  0x0048: 'Oven Cavity Operational State',
  0x0049: 'Oven Mode',
  0x004A: 'Laundry Dryer Controls',
  0x0050: 'Mode Select',
  0x0051: 'Laundry Washer Mode',
  0x0052: 'Refrigerator And Temperature Controlled Cabinet Mode',
  0x0053: 'Laundry Washer Controls',
  0x0054: 'RVC Run Mode',
  0x0055: 'RVC Clean Mode',
  0x0056: 'Temperature Control',
  0x0057: 'Refrigerator Alarm',
  0x0059: 'Dishwasher Mode',
  0x005B: 'Air Quality',
  0x005C: 'Smoke CO Alarm',
  0x005D: 'Dishwasher Alarm',
  0x005E: 'Microwave Oven Mode',
  0x005F: 'Microwave Oven Control',
  0x0060: 'Operational State',
  0x0061: 'RVC Operational State',
  0x0062: 'Scenes Management',
  0x0065: 'Groupcast',
  0x0071: 'HEPA Filter Monitoring',
  0x0072: 'Activated Carbon Filter Monitoring',
  0x0079: 'Water Tank Level Monitoring',
  0x0080: 'Boolean State Configuration',
  0x0081: 'Valve Configuration and Control',
  0x0090: 'Electrical Power Measurement',
  0x0091: 'Electrical Energy Measurement',
  0x0094: 'Water Heater Management',
  0x0095: 'Commodity Price',
  0x0097: 'Messages',
  0x0098: 'Device Energy Management',
  0x0099: 'Energy EVSE',
  0x009B: 'Energy Preference',
  0x009C: 'Power Topology',
  0x009D: 'Energy EVSE Mode',
  0x009E: 'Water Heater Mode',
  0x009F: 'Device Energy Management Mode',
  0x00A0: 'Electrical Grid Conditions',
  0x0101: 'Door Lock',
  0x0102: 'Window Covering',
  0x0104: 'Closure Control',
  0x0105: 'Closure Dimension',
  0x0150: 'Service Area',
  0x0200: 'Pump Configuration and Control',
  0x0201: 'Thermostat',
  0x0202: 'Fan Control',
  0x0204: 'Thermostat User Interface Configuration',
  0x0300: 'Color Control',
  0x0301: 'Ballast Configuration',
  0x0400: 'Illuminance Measurement',
  0x0402: 'Temperature Measurement',
  0x0403: 'Pressure Measurement',
  0x0404: 'Flow Measurement',
  0x0405: 'Relative Humidity Measurement',
  0x0406: 'Occupancy Sensing',
  0x040C: 'Carbon Monoxide Concentration Measurement',
  0x040D: 'Carbon Dioxide Concentration Measurement',
  0x0413: 'Nitrogen Dioxide Concentration Measurement',
  0x0415: 'Ozone Concentration Measurement',
  0x042A: 'PM2.5 Concentration Measurement',
  0x042B: 'Formaldehyde Concentration Measurement',
  0x042C: 'PM1 Concentration Measurement',
  0x042D: 'PM10 Concentration Measurement',
  0x042E: 'Total Volatile Organic Compounds Concentration Measurement',
  0x042F: 'Radon Concentration Measurement',
  0x0430: 'Soil Measurement',
  0x0431: 'Ambient Context Sensing',
  0x0451: 'Wi-Fi Network Management',
  0x0452: 'Thread Border Router Management',
  0x0453: 'Thread Network Directory',
  0x0503: 'Wake on LAN',
  0x0504: 'Channel',
  0x0505: 'Target Navigator',
  0x0506: 'Media Playback',
  0x0507: 'Media Input',
  0x0508: 'Low Power',
  0x0509: 'Keypad Input',
  0x050A: 'Content Launcher',
  0x050B: 'Audio Output',
  0x050C: 'Application Launcher',
  0x050D: 'Application Basic',
  0x050E: 'Account Login',
  0x050F: 'Content Control',
  0x0510: 'Content App Observer',
  0x0550: 'Zone Management',
  0x0551: 'Camera AV Stream Management',
  0x0552: 'Camera AV Settings User Level Management',
  0x0553: 'WebRTC Transport Provider',
  0x0554: 'WebRTC Transport Requestor',
  0x0555: 'Push AV Stream Transport',
  0x0556: 'Chime',
  0x0700: 'Commodity Tariff',
  0x0750: 'Ecosystem Information',
  0x0751: 'Commissioner Control',
  0x0752: 'Joint Fabric Datastore',
  0x0753: 'Joint Fabric Administrator',
  0x0801: 'TLS Certificate Management',
  0x0802: 'TLS Client Management',
  0x0B06: 'Meter Identification',
  0x0B07: 'Commodity Metering',
  0xFFF1FC05: 'Unit Testing',
  0xFFF1FC06: 'Fault Injection',
  0xFFF1FC20: 'Sample MEI',
}
const timeZones = [
  'UTC', 'Europe/Berlin', 'Europe/London', 'Europe/Paris', 'Europe/Madrid', 'Europe/Rome',
  'Europe/Stockholm', 'Europe/Helsinki', 'Europe/Warsaw', 'Europe/Istanbul',
  'America/New_York', 'America/Chicago', 'America/Denver', 'America/Los_Angeles',
  'America/Toronto', 'America/Vancouver', 'America/Sao_Paulo',
  'Asia/Dubai', 'Asia/Kolkata', 'Asia/Bangkok', 'Asia/Singapore', 'Asia/Shanghai',
  'Asia/Tokyo', 'Asia/Seoul', 'Australia/Sydney', 'Pacific/Auckland',
]
const browserTimeZone = Intl.DateTimeFormat().resolvedOptions().timeZone
const defaultTimeZone = timeZones.includes(browserTimeZone) ? browserTimeZone : 'Europe/Berlin'
const namedId = (id: number, names: Record<number, string>) =>
  `0x${id.toString(16).padStart(4, '0').toUpperCase()}${names[id] ? ` — ${names[id]}` : ''}`
function LightBulbIcon({ on, level = on ? 1 : 0 }: { on: boolean; level?: number }) {
  return (
    <svg viewBox="0 0 64 64" fill="none" stroke="currentColor" strokeWidth="3" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
      <path d="M21 40c0-5-7-9-7-18a18 18 0 0 1 36 0c0 9-7 13-7 18H21Z" fill="currentColor" fillOpacity={0.08 + 0.22 * level} />
      <path d="M24 46h16M26 52h12M29 57h6" />
      {on && <path d="M32 1v4M7 11l4 3M57 11l-4 3M3 27h5M61 27h-5" opacity={level} />}
    </svg>
  )
}

function blendColor(off: [number, number, number], on: [number, number, number], level: number) {
  return `rgb(${off.map((channel, index) => Math.round(channel + (on[index] - channel) * level)).join(', ')})`
}

export function LightGroupStates({ members, devices }: { members: string[]; devices: Device[] }) {
  return (
    <div className="group-light-states" role="group" aria-label="Light states">
      {members.map((address) => {
        const device = devices.find((item) => item.ieee_address === address && item.has_light)
        const state = device ? (device.light_on ? 'on' : 'off') : 'unknown'
        const brightness = device?.has_brightness ? Math.max(0, Math.min(100, device.brightness_percent)) : 100
        const level = device?.light_on ? brightness / 100 : 0
        const label = `${device?.name || address}: ${device ? (device.light_on ? 'On' : 'Off') : 'State unavailable'}`
        const style = device ? {
          color: blendColor([84, 117, 139], [173, 116, 0], level),
          backgroundColor: blendColor([237, 244, 249], [255, 241, 203], level),
          borderColor: blendColor([220, 229, 237], [243, 216, 139], level),
        } : undefined
        return <span key={address} className={`light-state-bulb ${state}`} role="img" title={label} aria-label={label} style={style}><LightBulbIcon on={!!device?.light_on} level={level} /></span>
      })}
    </div>
  )
}

function DeviceVisual({ device, type }: { device: Device; type: string }) {
  if (type === 'Water leak detectors') return <span aria-hidden="true">{device.water_detected ? '💧' : '◌'}</span>
  if (type === 'Contact sensors' && device.contact_trigger_mode) return (
    <svg viewBox="0 0 64 64" fill="none" stroke="currentColor" strokeWidth="3" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
      <path d="M24 49h16M27 55h10M29 10a3 3 0 0 1 6 0c9 2 14 10 14 20 0 8 3 12 6 15H9c3-3 6-7 6-15 0-10 5-18 14-20Z" fill="currentColor" fillOpacity=".12" />
      <circle cx="32" cy="49" r="2" fill="currentColor" stroke="none" />
    </svg>
  )
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
  if (type === 'Thermostats') return <span aria-hidden="true">🌡️</span>
  if (type === 'Lights') return <LightBulbIcon on={device.light_on} />
  if (type === 'Smart plugs' || type === 'Switches') return <span aria-hidden="true">{type === 'Smart plugs' ? '🔌' : '⏻'}</span>
  if (type === 'Motion sensors') return <span aria-hidden="true">{device.occupied ? '◉' : '◎'}</span>
  if (type === 'Presence sensors') return <span aria-hidden="true">{device.occupied ? '●' : '○'}</span>
  if (type === 'Air quality sensors') return <span aria-hidden="true">🫧</span>
  return <span aria-hidden="true">{type === 'Remotes' ? '🎛️' : '📡'}</span>
}
export const deviceTypes = [
  { name: 'Thermostats', matches: (device: Device) => !!device.has_thermostat },
  { name: 'Water leak detectors', matches: (device: Device) => device.has_water_leak },
  { name: 'Contact sensors', matches: (device: Device) => device.has_contact && !device.has_water_leak },
  {
    name: 'Climate sensors',
    matches: (device: Device) => !device.has_thermostat && !device.has_light && (device.has_temperature || device.has_humidity),
  },
  { name: 'Air quality sensors', matches: (device: Device) => !device.has_light &&
    (device.has_air_quality || device.has_co2 || device.has_pm25) },
  { name: 'Presence sensors', matches: (device: Device) => device.source === 'fritz_box' && device.has_occupancy },
  { name: 'Motion sensors', matches: (device: Device) => device.has_occupancy && device.source !== 'fritz_box' },
  { name: 'Lights', matches: (device: Device) => device.has_light },
  { name: 'Remotes', matches: (device: Device) => device.has_remote },
  { name: 'Switches', matches: (device: Device) => device.has_actor && !device.has_power },
  { name: 'Smart plugs', matches: (device: Device) => device.has_power },
  { name: 'Other devices', matches: (_device: Device) => true },
]
export const getDeviceType = (device: Device) =>
  deviceTypes.find((type) => type.matches(device))?.name ?? 'Other devices'
const byName = (left: Device, right: Device) =>
  left.name.localeCompare(right.name, undefined, { sensitivity: 'base', numeric: true }) ||
  left.id.localeCompare(right.id)
function relativeTime(epoch: number): string {
  if (!epoch) return 'Never'
  const seconds = Math.max(0, Math.floor(Date.now() / 1000 - epoch))
  if (seconds < 15) return 'just now'
  if (seconds < 60) return `${seconds} sec ago`
  if (seconds < 3600) return `${Math.floor(seconds / 60)} min ago`
  if (seconds < 86400) return `${Math.floor(seconds / 3600)} hr ago`
  return `${Math.floor(seconds / 86400)} days ago`
}

type SensorSeverity = 'good' | 'fair' | 'moderate' | 'poor' | 'very-poor' | 'extremely-poor'
type CardReading = { text: string; severity?: SensorSeverity }

function sensorSeverity(sensor: string, value: number | string | null, unit: string, quality: 'measured' | 'unknown'): SensorSeverity | undefined {
  if (quality !== 'measured' || value === null) return undefined
  if (sensor === 'air_quality' && typeof value === 'string') {
    const qualities: Record<string, SensorSeverity> = {
      good: 'good', fair: 'fair', moderate: 'moderate', poor: 'poor',
      very_poor: 'very-poor', extremely_poor: 'extremely-poor',
    }
    return qualities[value]
  }
  if (typeof value !== 'number') return undefined
  let normalized: number | undefined
  let limits: number[] | undefined
  if (sensor === 'co2') {
    normalized = unit === 'ppm' ? value : unit === 'ppb' ? value / 1000 : unit === 'ppt' ? value / 1000000 : undefined
    limits = [800, 1000, 1400, 2000, 5000]
  } else if (sensor === 'pm25') {
    normalized = unit === 'ug/m3' ? value : unit === 'mg/m3' ? value * 1000 : unit === 'ng/m3' ? value / 1000 : undefined
    limits = [10, 20, 35, 55, 125]
  }
  if (normalized === undefined || !limits) return undefined
  const severities: SensorSeverity[] = ['good', 'fair', 'moderate', 'poor', 'very-poor', 'extremely-poor']
  const band = limits.findIndex(limit => normalized < limit)
  return severities[band === -1 ? severities.length - 1 : band]
}

function cardSensorReading(sensor: string, value: number | string | null, unit: string, quality: 'measured' | 'unknown', colorize: boolean): CardReading {
  const label = ({ air_quality: 'Air quality', co2: 'CO₂', pm25: 'PM2.5' } as Record<string, string>)[sensor] ?? 'Measurement'
  const unknown = quality === 'unknown' || value === null
  return {
    text: `${label}: ${unknown ? 'Unknown' : `${value}${unit ? ` ${unit}` : ''}`}`,
    severity: colorize ? sensorSeverity(sensor, value, unit, quality) : undefined,
  }
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
export function DescriptorList({ descriptor, source }: { descriptor: EndpointDescriptor; source?: Device['source'] }) {
  const names = source === 'matter' ? matterClusterNames : clusterNames
  return (
    <section className="descriptor-row">
      <h3>Endpoint {descriptor.endpoint}</h3>
      {descriptor.profile_id !== undefined && descriptor.device_id !== undefined && <p className="mono muted">
        Profile {namedId(descriptor.profile_id, profileNames)} · Device{' '}
        {namedId(descriptor.device_id, deviceNames)} · Version {descriptor.device_version}
      </p>}
      <div className="descriptor-columns">
        <div>
          <b>Server / input clusters</b>
          <ul>
            {descriptor.input_clusters.map((id) => (
              <li key={id}>{namedId(id, names)}</li>
            ))}
            {!descriptor.input_clusters.length && <li>None</li>}
          </ul>
        </div>
        <div>
          <b>Client / output clusters</b>
          <ul>
            {descriptor.output_clusters.map((id) => (
              <li key={id}>{namedId(id, names)}</li>
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
  temperatureSources,
  action,
  onSettings,
  onDetails,
  onHistory,
  onConnection,
}: {
  device: Device
  temperatureSources: TemperatureSource[]
  action: (url: string, body?: string) => void
  onSettings: () => void
  onDetails: () => void
  onHistory: () => void
  onConnection: () => void
}) {
  const [brightness, setBrightness] = useState(
    device.has_brightness && device.brightness_known !== false ? device.brightness_percent : 100
  )
  useEffect(
    () => setBrightness(device.has_brightness && device.brightness_known !== false ? device.brightness_percent : 100),
    [device.brightness_percent, device.has_brightness, device.brightness_known]
  )
  const url = `/api/devices/${encodeURIComponent(device.id)}`
  const canDim = isDimmable(device)
  const controlsAvailable = device.online !== false && device.controller_available !== false
  const type = getDeviceType(device)
  const colorizeSensorReadings = device.has_air_quality || device.has_co2 || device.has_pm25
  const extraReadings = [
    type !== 'Climate sensors' && device.has_temperature && { text: `🌡 Temperature: ${device.temperature_known ? `${device.temperature_celsius} °C` : 'Unknown'}` },
    type !== 'Climate sensors' && device.has_humidity && { text: `💧 Humidity: ${device.humidity_known === false ? 'Unknown' : `${device.humidity_percent}%`}` },
    type !== 'Contact sensors' && device.has_contact && { text: device.contact_trigger_mode
      ? (device.contact_trigger_active ? 'Triggered' : 'Idle')
      : (device.contact_open ? 'Open' : 'Closed') },
    type !== 'Motion sensors' && type !== 'Presence sensors' && device.has_occupancy && { text: device.occupied ? 'Motion' : 'Clear' },
    type !== 'Presence sensors' && device.source === 'fritz_box' && device.has_wifi_signal && { text: `Wi‑Fi ${device.wifi_signal_percent}%` },
    type !== 'Lights' && device.has_light && { text: device.light_on ? 'Light on' : 'Light off' },
    type !== 'Smart plugs' && device.has_power && device.power_known && { text: `${device.power_watts.toFixed(1)} W` },
    ...Object.entries(device.sensor_values ?? {}).filter(([sensor]) => !['temperature', 'humidity'].includes(sensor))
      .map(([sensor, reading]) => cardSensorReading(sensor, reading.value, reading.unit, reading.quality, colorizeSensorReadings)),
    !device.sensor_values?.air_quality && device.has_air_quality && cardSensorReading('air_quality', device.air_quality_known ? device.air_quality : null, '', device.air_quality_known ? 'measured' : 'unknown', colorizeSensorReadings),
    !device.sensor_values?.co2 && device.has_co2 && cardSensorReading('co2', device.co2_known ? device.co2_ppm : null, 'ppm', device.co2_known ? 'measured' : 'unknown', colorizeSensorReadings),
    !device.sensor_values?.pm25 && device.has_pm25 && cardSensorReading('pm25', device.pm25_known ? device.pm25_micrograms_m3 : null, 'ug/m3', device.pm25_known ? 'measured' : 'unknown', colorizeSensorReadings),
  ].filter(Boolean) as CardReading[]
  return (
    <article className={`device-card device-card-${type.toLowerCase().replace(/[^a-z]+/g, '-')} ${
      ((type === 'Contact sensors' && !device.contact_trigger_mode && device.contact_open) ||
      (type === 'Water leak detectors' && device.water_leak_known && device.water_detected)) ||
      (type === 'Motion sensors' && device.occupied) ||
      (type === 'Presence sensors' && device.occupied) ||
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
              <span className="reading-label"><span className="reading-icon" aria-hidden="true">🌡️</span>Temperature</span>
            <strong>{device.temperature_known ? device.temperature_celsius : 'Unknown'}{device.temperature_known && <span className="reading-unit">°C</span>}</strong>
            </div>
          )}
          {device.has_humidity && (
            <div className="climate-reading">
              <span className="reading-label"><span className="reading-icon" aria-hidden="true">💧</span>Humidity</span>
              <strong>{device.humidity_known === false ? 'Unknown' : device.humidity_percent}{device.humidity_known === false ? null : <span className="reading-unit">%</span>}</strong>
            </div>
          )}
        </div>
      ) : (
        <div className={`device-focus ${type === 'Lights' ? 'light-focus' : ''}`}>
          <span className="focus-icon"><DeviceVisual device={device} type={type} /></span>
          <div className="focus-copy">
            <strong>{type === 'Water leak detectors' ? (!device.water_leak_known ? 'Unknown' : device.water_detected ? 'Wet' : 'Dry')
              : type === 'Contact sensors' ? (device.contact_trigger_mode
              ? (device.contact_trigger_active ? 'Triggered' : 'Idle')
              : (!device.contact_known ? 'Unknown' : device.contact_open ? 'Open' : 'Closed'))
              : type === 'Presence sensors' ? (!device.last_seen || Date.now() / 1000 - device.last_seen > 90 ? 'Unknown' : device.occupied ? 'Present' : 'Not present')
              : type === 'Motion sensors' ? (device.occupied ? 'Occupied' : 'Clear')
              : type === 'Lights' ? (device.light_known === false ? 'Unknown' : device.light_on ? 'On' : 'Off')
              : type === 'Air quality sensors' ? 'Air quality readings'
              : type === 'Switches' || type === 'Smart plugs' ? (!device.actor_known ? 'Unknown' : device.actor_on ? 'On' : 'Off')
              : type === 'Remotes' ? 'Remote' : device.kind}</strong>
            <span>{type === 'Water leak detectors' ? (device.contact_trigger_mode ? 'Water leak trigger mode' : 'Water leak detector')
              : type === 'Contact sensors' ? (device.contact_trigger_mode ? 'Trigger mode' : 'Contact sensor')
              : type === 'Presence sensors' ? 'Wi‑Fi presence sensor'
              : type === 'Motion sensors' ? 'Motion sensor'
              : type === 'Lights' ? 'Light'
              : type === 'Air quality sensors' ? 'Air quality sensor'
              : type === 'Switches' ? 'Switch'
              : type === 'Smart plugs' ? 'Smart plug'
              : type === 'Remotes' ? 'Control' : 'Device'}</span>
          </div>
        </div>
      )}
      {!!extraReadings.length && (
        <div className="extra-readings">
          {extraReadings.map((reading, index) => <span key={index} className={`sensor-reading${reading.severity ? ` sensor-reading-${reading.severity}` : ''}`}>{reading.text}</span>)}
        </div>
      )}
      {device.has_power && <div className="extra-readings"><span>Power: {device.power_known ? `${device.power_watts.toFixed(1)} W` : 'Unknown'}</span></div>}
      {device.has_actor && <div className="light-panel"><button className={`button light-toggle ${device.actor_known && device.actor_on ? 'primary' : ''}`} disabled={!controlsAvailable} onClick={() => action(`${url}/actor`, form({ state: device.actor_known && device.actor_on ? 'off' : 'on' }))}>{device.actor_known && device.actor_on ? 'Turn off' : 'Turn on'}</button></div>}
      {device.has_thermostat && <ThermostatPanel device={device} sources={temperatureSources} />}
      {device.has_light && (
        <div className="light-panel">
          <button
            className={`button light-toggle ${device.light_on ? 'primary' : ''}`}
            disabled={!controlsAvailable}
            onClick={() => action(`${url}/light`, form({ state: device.light_on ? 'off' : 'on' }))}
          >
            {device.light_on ? 'Turn off' : 'Turn on'}
          </button>
          {canDim && (
            <>
              <div className="brightness-row">
                <label htmlFor={`brightness-${device.id}`}>Brightness</label>
                <span>{device.brightness_known === false ? 'Unknown' : `${brightness}%`}</span>
              </div>
              <input
                id={`brightness-${device.id}`}
                type="range"
                min="0"
                max="100"
                value={brightness}
                disabled={!controlsAvailable}
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
                  <button className="button" key={level} disabled={!controlsAvailable} onClick={() => action(`${url}/light`, form({ brightness: level }))}>
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
          {device.source === 'zigbee' && <span title="Link quality">LQI {device.link_quality}</span>}
          {device.source === 'matter' && <span title="Direct Thread signal">RSSI {device.matter_rssi_dbm === null || device.matter_rssi_dbm === undefined ? 'unavailable' : `${device.matter_rssi_dbm} dBm`}</span>}
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
          <button title="Settings" aria-label={`Settings for ${device.name}`} onClick={onSettings}>⚙</button>
          {(device.has_temperature || device.has_humidity || device.has_contact || device.has_power || device.has_occupancy ||
            device.has_battery || device.has_co2 || device.has_pm25 || device.has_air_quality || device.has_light ||
            device.has_actor || device.has_brightness) && (
            <button title="History" aria-label={`History for ${device.name}`} onClick={onHistory}>
              <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
                <path d="M3 3v18h18" />
                <path d="m6 16 4-5 4 3 5-7" />
              </svg>
            </button>
          )}
          {(device.source === 'shelly' || device.source === 'fritz_box') && <button title="External device setup" aria-label={`Connection setup for ${device.name}`} onClick={onConnection}>⌁</button>}
          <button
            title="Refresh attributes"
            aria-label={`Refresh ${device.name}`}
            disabled={!controlsAvailable}
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
  onConnection,
  onlyType,
  hideContext,
}: {
  status: Status | null
  action: (url: string, body?: string) => void
  onSettings: (device: Device) => void
  onDetails: (device: Device) => void
  onHistory: (device: Device) => void
  onConnection: (device: Device) => void
  onlyType?: string
  hideContext?: boolean
}) {
  if (!status?.devices.length) return <div className="empty">No devices configured or discovered yet.</div>
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
      {deviceTypes.filter((type) => !onlyType || type.name === onlyType).map((type) => {
        const devices = status.devices
          .filter((device) => getDeviceType(device) === type.name)
          .sort(byName)
        if (!devices.length) return null
        const sections = hideContext
          ? [{ id: 'dashboard', name: '', devices }]
          : type.name === 'Lights'
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
            {!hideContext && <div className="device-type-heading">
              <h3>{type.name}</h3>
              <span>{devices.length}</span>
            </div>}
            {sections.map((section) => (
              <div className="device-subgroup" key={section.id}>
                {!hideContext && type.name === 'Lights' && (
                  <h4 className="device-subgroup-heading">{section.name}</h4>
                )}
                <div className="device-grid">
                  {section.devices.map((device) => (
                    <DeviceCard
                      key={device.id}
                      device={device}
                      temperatureSources={status.temperature_sources || []}
                      action={action}
                      onSettings={() => onSettings(device)}
                      onDetails={() => onDetails(device)}
                      onHistory={() => onHistory(device)}
                      onConnection={() => onConnection(device)}
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
  onDeleted,
}: {
  device: Device
  onClose: () => void
  onSaved: () => void
  onDeleted?: () => void
}) {
  const co2Unit = device.sensor_values?.co2?.unit ?? ''
  const pm25Unit = device.sensor_values?.pm25?.unit ?? ''
  const alertMetrics = [
    { id: 'temperature', label: 'Temperature', unit: '°C', available: device.has_temperature },
    { id: 'humidity', label: 'Humidity', unit: '%', available: device.has_humidity },
    { id: 'battery', label: 'Battery', unit: '%', available: device.has_battery },
    { id: 'link_quality', label: 'Link quality', unit: 'LQI', available: device.source === 'zigbee' },
    { id: 'wifi_signal', label: 'Wi-Fi signal', unit: '%', available: device.has_wifi_signal },
    { id: 'brightness', label: 'Brightness', unit: '%', available: device.has_brightness },
    { id: 'power', label: 'Power', unit: 'W', available: device.has_power },
    { id: 'co2', label: 'CO₂', unit: co2Unit, available: device.has_co2 && Boolean(co2Unit) },
    { id: 'pm25', label: 'PM2.5', unit: pm25Unit, available: device.has_pm25 && Boolean(pm25Unit) },
  ]
  const initialAlert = (metric: string, field: 'low' | 'high' | 'hysteresis') => {
    const alert = device.value_alerts?.[metric]
    if (alert) {
      if ((metric === 'co2' && alert.unit !== co2Unit) || (metric === 'pm25' && alert.unit !== pm25Unit)) return ''
      if (field === 'low') return alert.has_low ? String(alert.low) : ''
      if (field === 'high') return alert.has_high ? String(alert.high) : ''
      return String(alert.hysteresis)
    }
    if (metric === 'temperature' && field === 'low' && device.has_low_temperature_alert) return String(device.low_temperature_alert)
    if (metric === 'temperature' && field === 'high' && device.has_high_temperature_alert) return String(device.high_temperature_alert)
    return field === 'hysteresis' ? '0' : ''
  }
  const [deleting, setDeleting] = useState(false)
  const [name, setName] = useState(device.name)
  const [alertValues, setAlertValues] = useState<Record<string, { low: string; high: string; hysteresis: string }>>(
    Object.fromEntries(alertMetrics.map(metric => [metric.id, {
      low: initialAlert(metric.id, 'low'), high: initialAlert(metric.id, 'high'), hysteresis: initialAlert(metric.id, 'hysteresis'),
    }]))
  )
  const [initial, setInitial] = useState(String(device.contact_initial_alert_minutes))
  const [reminder, setReminder] = useState(String(device.contact_reminder_alert_minutes))
  const [triggerMode, setTriggerMode] = useState(device.contact_trigger_mode)
  const [triggerCooldown, setTriggerCooldown] = useState(String(device.contact_trigger_cooldown_seconds))
  const [refresh, setRefresh] = useState(String(device.sensor_refresh_interval_minutes))
  const [error, setError] = useState('')
  const [saving, setSaving] = useState(false)
  const [syncingClock, setSyncingClock] = useState(false)
  const [clockResult, setClockResult] = useState('')
  const [timeZone, setTimeZone] = useState(defaultTimeZone)
  const [subscriptions, setSubscriptions] = useState<MatterSubscriptionHealth>()
  const [subscriptionBusy, setSubscriptionBusy] = useState(false)
  const [subscriptionResult, setSubscriptionResult] = useState('')
  const loadSubscriptions = async (restore = false) => {
    if (!device.matter_node || subscriptionBusy) return
    setSubscriptionBusy(true)
    setSubscriptionResult('')
    try {
      setSubscriptions(await matterSubscriptions(device.matter_node, restore, new AbortController().signal))
      if (restore) setSubscriptionResult('Recovery scheduled. Refresh to check progress.')
    } catch (cause) {
      setSubscriptionResult(errorMessage(cause))
    } finally {
      setSubscriptionBusy(false)
    }
  }
  const clockEndpoint = device.source === 'matter'
    ? device.endpoint_descriptors.find(endpoint => endpoint.input_clusters.includes(56))?.endpoint
    : undefined
  const syncClock = async () => {
    if (!device.matter_node || clockEndpoint === undefined || syncingClock) return
    setSyncingClock(true)
    setClockResult('')
    try {
      await matterSyncTime(device.matter_node, clockEndpoint, timeZone, new AbortController().signal)
      setClockResult('Clock synchronized.')
    } catch (cause) {
      setClockResult(errorMessage(cause))
    } finally {
      setSyncingClock(false)
    }
  }
  const save = async (event: React.FormEvent) => {
    event.preventDefault()
    setSaving(true)
    setError('')
    const fields: Record<string, string> = { name: name.trim() }
    if (device.has_temperature) { fields.low = ''; fields.high = '' }
    for (const metric of alertMetrics) {
      if (!metric.available) continue
      const value = alertValues[metric.id]
      fields[`value_alert_${metric.id}_low`] = value.low
      fields[`value_alert_${metric.id}_high`] = value.high
      fields[`value_alert_${metric.id}_hysteresis`] = value.hysteresis
      if (metric.id === 'co2' || metric.id === 'pm25') fields[`value_alert_${metric.id}_unit`] = metric.unit
    }
    if (device.has_contact || device.has_water_leak) {
      fields.initial_minutes = initial
      fields.reminder_minutes = reminder
      fields.contact_trigger_mode = String(triggerMode)
      fields.contact_trigger_cooldown_seconds = triggerCooldown
    }
    if (device.source === 'zigbee' && (device.has_temperature || device.has_humidity)) fields.sensor_refresh_minutes = refresh
    try {
      await post(`/api/devices/${encodeURIComponent(device.id)}/settings`, form(fields))
      onSaved()
      onClose()
    } catch (e) {
      setError(errorMessage(e))
    } finally {
      setSaving(false)
    }
  }
  if (deleting) return <DeleteDeviceDialog device={device} onClose={() => setDeleting(false)} onDeleted={() => { (onDeleted ?? onSaved)(); onClose() }} />
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
        {(device.source === 'zigbee' || device.source === 'matter') && <OtaUpdate device={device} />}
        <label className="field">
          Name
          <input
            value={name}
            onChange={(event) => setName(event.target.value)}
            maxLength={64}
            required
          />
        </label>
        {(device.has_contact || device.has_water_leak) && (
          <>
            <label className="check">
              <input type="checkbox" checked={triggerMode} onChange={(event) => setTriggerMode(event.target.checked)} /> {device.has_water_leak ? 'Water leak trigger mode' : 'Trigger mode'}
            </label>
            {triggerMode ? (
              <label className="field">
                Cooldown (seconds)
                <input type="number" min="1" step="1" value={triggerCooldown} onChange={(event) => setTriggerCooldown(event.target.value)} required />
              </label>
            ) : (
              <>
                <label className="field">
                  First alert after (minutes)
                  <input type="number" min="1" step="1" value={initial} onChange={(event) => setInitial(event.target.value)} required />
                </label>
                <label className="field">
                  Reminder every (minutes)
                  <input type="number" min="1" step="1" value={reminder} onChange={(event) => setReminder(event.target.value)} required />
                </label>
              </>
            )}
          </>
        )}
        {alertMetrics.filter(metric => metric.available).map(metric => (
          <fieldset className="field" key={metric.id}>
            <legend>{metric.label} alerts ({metric.unit})</legend>
            <label>Lower limit (blank disables)
              <input type="number" step="any" value={alertValues[metric.id].low}
                onChange={event => setAlertValues(values => ({ ...values, [metric.id]: { ...values[metric.id], low: event.target.value } }))} />
            </label>
            <label>Upper limit (blank disables)
              <input type="number" step="any" value={alertValues[metric.id].high}
                onChange={event => setAlertValues(values => ({ ...values, [metric.id]: { ...values[metric.id], high: event.target.value } }))} />
            </label>
            <label>Hysteresis
              <input type="number" min="0" step="any" value={alertValues[metric.id].hysteresis}
                onChange={event => setAlertValues(values => ({ ...values, [metric.id]: { ...values[metric.id], hysteresis: event.target.value } }))} />
            </label>
          </fieldset>
        ))}
          {device.source === 'zigbee' && (device.has_temperature || device.has_humidity) && (
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
        {device.source === 'matter' && device.matter_node && (
          <fieldset>
            <legend>Measurement subscriptions</legend>
            <p className="muted">Reports recover automatically after their maximum interval plus 10 seconds. Sleeping devices wait until they wake.</p>
            {subscriptions && <>
              <p>Device: {subscriptions.reachability}{subscriptions.intermittent ? ' (supports intermittent connectivity)' : ''}</p>
              {subscriptions.subscriptions.length === 0 && <p>No subscriptions currently tracked.</p>}
              {subscriptions.subscriptions.map(row => <p key={`${row.endpoint}:${row.cluster}:${row.attribute}`}>
                Endpoint {row.endpoint} · {clusterNames[row.cluster] ?? `Cluster ${row.cluster}`} · {row.state}<br />
                <span className="muted">ID {row.id ?? 'pending'} · Maximum interval {row.max_interval_seconds}s · Last report {row.last_report_age_seconds === null ? 'not yet received' : `${row.last_report_age_seconds}s ago`} · Retry/check in {row.retry_in_seconds}s · Failures {row.failures}</span>
              </p>)}
            </>}
            {subscriptionResult && <p role="status">{subscriptionResult}</p>}
            <button type="button" className="button" disabled={subscriptionBusy} onClick={() => void loadSubscriptions()}>
              {subscriptionBusy ? 'Loading…' : 'Refresh subscription health'}
            </button>
            <button type="button" className="button" disabled={subscriptionBusy} onClick={() => void loadSubscriptions(true)}>
              Restore subscriptions
            </button>
          </fieldset>
        )}
        {clockEndpoint !== undefined && (
          <fieldset>
            <legend>Clock</legend>
            <p className="muted">Set the accessory clock from the gateway’s UTC time.</p>
            <label className="field">Time zone
              <select value={timeZone} onChange={(event) => setTimeZone(event.target.value)}>
                {timeZones.map(zone => <option key={zone} value={zone}>{zone}</option>)}
              </select>
            </label>
            {clockResult && <p className={clockResult === 'Clock synchronized.' ? 'success' : 'error'} role="status">{clockResult}</p>}
            <button type="button" className="button" disabled={syncingClock || !device.online} onClick={() => void syncClock()}>
              {syncingClock ? 'Synchronizing…' : 'Synchronize clock now'}
            </button>
          </fieldset>
        )}
        <div className="dialog-actions">
          {['zigbee', 'matter'].includes(device.source) && <button type="button" className="button danger" disabled={saving} onClick={() => setDeleting(true)}>Delete device</button>}
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

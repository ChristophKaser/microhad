import { useEffect, useState } from 'react'
import type { Device, TemperatureSource } from './api'
import { errorMessage, form, post } from './api'
import { ThermostatDialog } from './ThermostatDialog'

export type Setting = {
  field: string; label: string; min?: number; max?: number; step?: number
  options?: [number, string][]; unit?: string
}
const settings: Setting[] = [
  { field: 'occupied_heating_setpoint', label: 'Heating target', min: 4, max: 35, step: .5, unit: '°C' },
  { field: 'system_mode', label: 'Operating mode', options: [[0, 'Off'], [1, 'Auto (schedule)'], [4, 'Heat']] },
  { field: 'occupied_cooling_setpoint', label: 'Cooling target', unit: '°C' },
  { field: 'unoccupied_heating_setpoint', label: 'Unoccupied heating target', unit: '°C' },
  { field: 'unoccupied_cooling_setpoint', label: 'Unoccupied cooling target', unit: '°C' },
  ...['min_heat_setpoint_limit', 'max_heat_setpoint_limit', 'min_cool_setpoint_limit', 'max_cool_setpoint_limit', 'min_setpoint_dead_band', 'remote_sensing', 'programming_operation_mode'].map(field => ({ field, label: field.replace(/_/g, ' ') })),
  { field: 'local_temperature_calibration', label: 'Temperature calibration', min: -12.7, max: 12.7, step: .1, unit: '°C' },
  { field: 'child_lock', label: 'Child lock', options: [[0, 'Unlocked'], [1, 'Locked']] },
  { field: 'open_window', label: 'Window detection', options: [[0, 'Off'], [1, 'On']] },
  { field: 'frost_protection_temperature', label: 'Frost protection', min: 4, max: 35, step: .5, unit: '°C' },
  { field: 'external_temperature_input', label: 'Manual external temperature', min: 0, max: 99.9, step: .1, unit: '°C' },
  { field: 'valve_opening_degree', label: 'Valve opening limit', min: 0, max: 100, step: 1, unit: '%' },
  { field: 'valve_closing_degree', label: 'Valve closing limit', min: 0, max: 100, step: 1, unit: '%' },
  { field: 'temperature_accuracy', label: 'Control accuracy', options: [[-1, '−1 °C'], [-.8, '−0.8 °C'], [-.6, '−0.6 °C'], [-.4, '−0.4 °C'], [-.2, '−0.2 °C']] },
  { field: 'temporary_mode_select', label: 'Temporary mode', options: [[255, 'Inactive'], [0, 'Boost'], [1, 'Timer']] },
  { field: 'temporary_mode_duration', label: 'Temporary duration', min: 0, max: 1440, step: 1, unit: 'minutes' },
  { field: 'timer_mode_target_temp', label: 'Timer target temperature', min: 4, max: 35, step: .5, unit: '°C' },
  { field: 'smart_temperature_control', label: 'Smart temperature control', options: [[0, 'Off'], [2, 'On']] },
]
const diagnostics = [
  ['idle_steps', 'Idle steps'], ['closing_steps', 'Closing steps'],
  ['valve_opening_limit_voltage', 'Opening limit (mV)'],
  ['valve_closing_limit_voltage', 'Closing limit (mV)'], ['valve_motor_running_voltage', 'Motor voltage (mV)'],
]
function SettingInput({ setting, reported, disabled, send }: {
  setting: Setting; reported: number | undefined; disabled: boolean
  send: (field: string, value: string) => Promise<void>
}) {
  const [value, setValue] = useState(reported === undefined ? '' : String(reported))
  useEffect(() => setValue(reported === undefined ? '' : String(reported)), [reported])
  return <form className="thermostat-setting" onSubmit={event => { event.preventDefault(); void send(setting.field, value) }}>
    <label>{setting.label}{setting.unit && ` (${setting.unit})`}
      {setting.options ? <select value={value} disabled={disabled} onChange={event => setValue(event.target.value)}>
        <option value="" disabled>Unknown</option>
        {setting.options.map(([key, label]) => <option key={key} value={key} disabled={setting.field === 'temporary_mode_select' && key === 255}>{label}</option>)}
      </select> : <input type="number" required placeholder="Unknown" value={value} min={setting.min} max={setting.max} step={setting.step}
        disabled={disabled} onChange={event => setValue(event.target.value)} />}
    </label>
    <button className="button" disabled={disabled || value === '' || value === String(reported)}>Apply</button>
  </form>
}
export function ThermostatPanel({ device, sources }: { device: Device; sources: TemperatureSource[] }) {
  const t = device.thermostat!
  const [busy, setBusy] = useState(false)
  const [message, setMessage] = useState('')
  const [error, setError] = useState('')
  const [open, setOpen] = useState(false)
  const disabled = busy || device.controller_available === false
  async function send(field: string, value: string) {
    setBusy(true); setError(''); setMessage('')
    try {
      await post(`/api/devices/${encodeURIComponent(device.id)}/thermostat`, form({ field, value }))
      setMessage(field === 'stale_minutes' ? 'Timeout saved.' : 'Request queued. Awaiting device confirmation.')
    } catch (reason) { setError(errorMessage(reason)); throw reason }
    finally { setBusy(false) }
  }
  const sendSetting = async (field: string, value: string) => { try { await send(field, value) } catch { /* displayed above */ } }
  function control(setting: Setting) {
    if (!t.supported[setting.field]) return null
    const unavailable = t.pending.includes(setting.field) || t.fields[setting.field]?.writable === false
    let adjusted = { ...setting, ...t.fields[setting.field] }
    if (setting.field === 'system_mode') adjusted = { ...adjusted, options: t.modes.map(mode => [mode, ({ 0: 'Off', 1: 'Auto', 3: 'Cool', 4: 'Heat' } as Record<number, string>)[mode] || `Mode ${mode}`]) }
    return <SettingInput key={setting.field} setting={adjusted} reported={t.values[setting.field]} disabled={disabled || unavailable} send={sendSetting} />
  }
  return <div className="thermostat-panel">
    {t.values.temperature_sensor_select === 2 && <p className="small">The valve reports its external source is offline and is using its built-in sensor.</p>}
    <p>Operation: {t.values.running_state === undefined ? (t.values.running_mode === undefined ? 'Unknown' : t.values.running_mode === 3 ? 'Cooling' : t.values.running_mode === 4 ? 'Heating' : 'Idle') : [t.values.running_state & 1 ? 'Heating' : '', t.values.running_state & 2 ? 'Cooling' : '', t.values.running_state & 4 ? 'Fan' : ''].filter(Boolean).join(', ') || 'Idle'}</p>
    {settings.slice(0, 3).map(control)}
    <p className="small" role="status">{t.status || 'Waiting for thermostat readings'}{message && ` · ${message}`}</p>
    {(error || t.error) && <p className="error" role="alert">{error || t.error}</p>}
    <button type="button" className="button" onClick={() => setOpen(true)}>Thermostat settings</button>
    {open && <ThermostatDialog device={device} sources={sources} settings={settings.slice(3)} diagnostics={diagnostics} onClose={() => setOpen(false)} />}
  </div>
}

import { useEffect, useState } from 'react'
import type { Device, TemperatureSource } from './api'
import { errorMessage, form, post } from './api'

type Setting = {
  field: string; label: string; min?: number; max?: number; step?: number
  options?: [number, string][]; unit?: string
}
const settings: Setting[] = [
  { field: 'occupied_heating_setpoint', label: 'Target temperature', min: 4, max: 35, step: .5, unit: '°C' },
  { field: 'system_mode', label: 'Heating mode', options: [[0, 'Off'], [1, 'Auto (schedule)'], [4, 'Heat']] },
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
const days = ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday']

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
function ScheduleDay({ day, reported, disabled, send }: {
  day: number; reported: string | null; disabled: boolean; send: (field: string, value: string) => Promise<void>
}) {
  const [draft, setDraft] = useState(reported || '')
  const [dirty, setDirty] = useState(false)
  useEffect(() => { if (!dirty) setDraft(reported || '') }, [reported, dirty])
  return <form className="thermostat-setting" onSubmit={event => {
    event.preventDefault(); void send(`schedule_${day}`, draft).then(() => setDirty(false)).catch(() => {})
  }}><label>{days[day]}<input required value={draft} placeholder="00:00/18 07:00/21 22:00/17" disabled={disabled}
    onChange={event => { setDraft(event.target.value); setDirty(true) }} /></label>
    <button className="button" disabled={disabled || !dirty}>Save day</button></form>
}
export function ThermostatPanel({ device, sources }: { device: Device; sources: TemperatureSource[] }) {
  const t = device.thermostat!
  const [busy, setBusy] = useState(false)
  const [message, setMessage] = useState('')
  const [error, setError] = useState('')
  const selectedSource = t.source_id || (t.values.temperature_sensor_select !== undefined && t.values.temperature_sensor_select !== 0 ? 'manual' : 'builtin')
  const [source, setSource] = useState(selectedSource)
  useEffect(() => setSource(selectedSource), [selectedSource])
  const [stale, setStale] = useState(String(t.stale_minutes))
  useEffect(() => setStale(String(t.stale_minutes)), [t.stale_minutes])
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
  const smart = t.values.smart_temperature_control === 2
  function control(setting: Setting) {
    const unavailable = !t.supported[setting.field] || t.pending.includes(setting.field) ||
      (!!t.source_id && setting.field === 'external_temperature_input') ||
      (smart && ['valve_opening_degree', 'temperature_accuracy'].includes(setting.field))
    const adjusted = setting.field === 'temporary_mode_duration' && t.values.temporary_mode_select !== 1 ? { ...setting, max: 180 } : setting
    return <SettingInput key={setting.field} setting={adjusted} reported={t.values[setting.field]} disabled={disabled || unavailable} send={sendSetting} />
  }
  return <div className="thermostat-panel">
    {t.values.temperature_sensor_select === 2 && <p className="small">The valve reports its external source is offline and is using its built-in sensor.</p>}
    <p>Heating: {t.values.running_state === undefined ? 'Unknown' : (t.values.running_state & 1) ? 'Heating' : 'Idle'}</p>
    {settings.slice(0, 2).map(control)}
    <p className="small" role="status">{t.status || 'Waiting for thermostat readings'}{message && ` · ${message}`}</p>
    {(error || t.error) && <p className="error" role="alert">{error || t.error}</p>}
    <details><summary>Thermostat settings</summary>
      <form className="thermostat-setting" onSubmit={event => { event.preventDefault(); void sendSetting('temperature_source', source) }}>
        <label>Temperature source<select value={source} disabled={disabled || !t.supported.temperature_sensor_select || t.pending.includes('temperature_sensor_select')} onChange={event => setSource(event.target.value)}>
          <option value="builtin">Built-in sensor</option><option value="manual">Manual / API input</option>
          {t.source_id && !sources.some(s => s.id === t.source_id) && <option value={t.source_id}>Missing sensor ({t.source_id})</option>}
          {sources.filter(s => s.id !== device.id).map(s => <option key={s.id} value={s.id}>{s.name || s.id}</option>)}
        </select></label><button className="button" disabled={disabled || !t.supported.temperature_sensor_select || t.pending.includes('temperature_sensor_select') || source === selectedSource}>Apply</button>
      </form>
      {t.source_id && <>
        <p className="small">Linked sensor: {sources.find(s => s.id === t.source_id)?.name || t.source_id}<br />
          Last temperature reading: {t.source_observed_at ? new Date(t.source_observed_at * 1000).toLocaleString() : 'Waiting for a fresh reading'}</p>
        <form className="thermostat-setting" onSubmit={event => { event.preventDefault(); void sendSetting('stale_minutes', stale) }}>
          <label>Fallback after (minutes)<input type="number" min="5" max="1440" step="1" required value={stale} onChange={event => setStale(event.target.value)} /></label>
          <button className="button" disabled={disabled || stale === String(t.stale_minutes)}>Save</button>
        </form>
        <p className="small">Uses the built-in sensor when the linked reading is missing, invalid or too old. Resumes automatically when fresh readings return.</p>
      </>}
      {settings.slice(2).map(control)}
      {smart && <p className="small">Disable smart control before changing the opening limit or control accuracy.</p>}
      <details><summary>Weekly schedule</summary><p className="small">Auto mode uses this schedule. Enter 1–6 transitions per day as HH:mm/temperature, starting at 00:00. Temperatures: 4–35 °C in 0.5 °C steps.</p>
        {days.map((_, day) => <ScheduleDay key={day} day={day} reported={t.schedules[day] ?? null} disabled={disabled || t.pending.includes(`schedule_${day}`)} send={send} />)}
      </details>
      <details><summary>Valve diagnostics</summary><dl>{diagnostics.map(([field, label]) => <div key={field}><dt>{label}</dt><dd>{t.values[field] ?? 'Unknown'}</dd></div>)}</dl></details>
    </details>
  </div>
}

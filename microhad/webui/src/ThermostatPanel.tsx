import { useEffect, useState } from 'react'
import type { Device, TemperatureSource, ThermostatSchedule } from './api'
import { errorMessage, form, post } from './api'

type Setting = {
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
function ScheduleDay({ day, reported, thermostat, disabled, send }: {
  day: number; reported: ThermostatSchedule | null; thermostat: Device['thermostat']; disabled: boolean
  send: (field: string, value: string) => Promise<void>
}) {
  const t = thermostat!
  const defaultMode = t.supported.occupied_heating_setpoint ? 1 : 2
  const [draft, setDraft] = useState<ThermostatSchedule>(reported || { mode: defaultMode, transitions: [] })
  const [dirty, setDirty] = useState(false)
  useEffect(() => { if (!dirty) setDraft(reported || { mode: defaultMode, transitions: [] }) }, [reported, dirty, defaultMode])
  const modes = [[1, 'Heating'], [2, 'Cooling'], [3, 'Heating and cooling']] as const
  function editTransition(index: number, field: 'time' | 'heat' | 'cool', value: number) {
    setDraft(current => ({ ...current, transitions: current.transitions.map((transition, i) => i === index ? { ...transition, [field]: value } : transition) })); setDirty(true)
  }
  return <form className="thermostat-day" onSubmit={event => {
    event.preventDefault(); void send(`schedule_${day}`, JSON.stringify(draft)).then(() => setDirty(false)).catch(() => {})
  }}><h4>{days[day]}</h4>
    <label>Schedule type<select value={draft.mode} disabled={disabled} onChange={event => {
      const mode = Number(event.target.value)
      setDraft(current => ({ mode, transitions: current.transitions.map(row => ({ time: row.time,
        ...(mode & 1 ? { heat: row.heat ?? 20 } : {}), ...(mode & 2 ? { cool: row.cool ?? 25 } : {}) })) })); setDirty(true)
    }}>{modes.filter(([mode]) => (!(mode & 1) || t.supported.occupied_heating_setpoint) && (!(mode & 2) || t.supported.occupied_cooling_setpoint)).map(([mode, label]) => <option key={mode} value={mode}>{label}</option>)}</select></label>
    {!reported && !dirty && <p className="small">Schedule not read yet.</p>}
    {draft.transitions.map((row, index) => <div className="thermostat-setting" key={index}>
      <label>Time<input type="time" required disabled={disabled} value={`${String(Math.floor(row.time / 60)).padStart(2, '0')}:${String(row.time % 60).padStart(2, '0')}`}
        onChange={event => { const [hour, minute] = event.target.value.split(':').map(Number); editTransition(index, 'time', hour * 60 + minute) }} /></label>
      {(['heat', 'cool'] as const).filter(field => draft.mode & (field === 'heat' ? 1 : 2)).map(field => {
        const metadata = t.fields[field === 'heat' ? 'occupied_heating_setpoint' : 'occupied_cooling_setpoint']
        return <label key={field}>{field === 'heat' ? 'Heat' : 'Cool'} (°C)<input type="number" required disabled={disabled} value={row[field] ?? ''}
          min={metadata?.min} max={metadata?.max} step={metadata?.step ?? .01} onChange={event => editTransition(index, field, Number(event.target.value))} /></label>
      })}
      <button type="button" className="button" disabled={disabled} aria-label={`Remove ${days[day]} transition ${index + 1}`} onClick={() => { setDraft(current => ({ ...current, transitions: current.transitions.filter((_, i) => i !== index) })); setDirty(true) }}>Remove</button>
    </div>)}
    <div className="thermostat-setting">
      <button type="button" className="button" disabled={disabled || draft.transitions.length >= t.schedule_max_transitions} onClick={() => {
        setDraft(current => ({ ...current, transitions: [...current.transitions, { time: current.transitions.length ? Math.min(1439, current.transitions[current.transitions.length - 1].time + 60) : 0,
          ...(current.mode & 1 ? { heat: 20 } : {}), ...(current.mode & 2 ? { cool: 25 } : {}) }] })); setDirty(true)
      }}>Add transition</button>
      <button type="button" className="button" disabled={disabled} onClick={() => { void send(`schedule_${day}`, '__read__').catch(() => {}) }}>Read day</button>
      <button className="button" disabled={disabled || !dirty || !draft.transitions.length}>Save day</button>
    </div>
  </form>
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
    if (!t.supported[setting.field]) return null
    const unavailable = !t.supported[setting.field] || t.pending.includes(setting.field) ||
      (!!t.source_id && setting.field === 'external_temperature_input') ||
      (smart && ['valve_opening_degree', 'temperature_accuracy'].includes(setting.field))
    let adjusted = { ...setting, ...t.fields[setting.field] }
    if (setting.field === 'system_mode') adjusted = { ...adjusted, options: t.modes.map(mode => [mode, ({ 0: 'Off', 1: 'Auto', 3: 'Cool', 4: 'Heat' } as Record<number, string>)[mode] || `Mode ${mode}`]) }
    if (setting.field === 'temporary_mode_duration' && t.values.temporary_mode_select !== 1) adjusted.max = 180
    return <SettingInput key={setting.field} setting={adjusted} reported={t.values[setting.field]} disabled={disabled || unavailable} send={sendSetting} />
  }
  return <div className="thermostat-panel">
    {t.values.temperature_sensor_select === 2 && <p className="small">The valve reports its external source is offline and is using its built-in sensor.</p>}
    <p>Operation: {t.values.running_state === undefined ? (t.values.running_mode === undefined ? 'Unknown' : t.values.running_mode === 3 ? 'Cooling' : t.values.running_mode === 4 ? 'Heating' : 'Idle') : [t.values.running_state & 1 ? 'Heating' : '', t.values.running_state & 2 ? 'Cooling' : '', t.values.running_state & 4 ? 'Fan' : ''].filter(Boolean).join(', ') || 'Idle'}</p>
    {settings.slice(0, 3).map(control)}
    <p className="small" role="status">{t.status || 'Waiting for thermostat readings'}{message && ` · ${message}`}</p>
    {(error || t.error) && <p className="error" role="alert">{error || t.error}</p>}
    <details><summary>Thermostat settings</summary>
      {t.external_input_supported && <form className="thermostat-setting" onSubmit={event => { event.preventDefault(); void sendSetting('temperature_source', source) }}>
        <label>Temperature source<select value={source} disabled={disabled || !t.supported.temperature_sensor_select || t.pending.includes('temperature_sensor_select')} onChange={event => setSource(event.target.value)}>
          <option value="builtin">Built-in sensor</option><option value="manual">Manual / API input</option>
          {t.source_id && !sources.some(s => s.id === t.source_id) && <option value={t.source_id}>Missing sensor ({t.source_id})</option>}
          {sources.filter(s => s.id !== device.id).map(s => <option key={s.id} value={s.id}>{s.name || s.id}</option>)}
        </select></label><button className="button" disabled={disabled || !t.supported.temperature_sensor_select || t.pending.includes('temperature_sensor_select') || source === selectedSource}>Apply</button>
      </form>}
      {t.source_id && <>
        <p className="small">Linked sensor: {sources.find(s => s.id === t.source_id)?.name || t.source_id}<br />
          Last temperature reading: {t.source_observed_at ? new Date(t.source_observed_at * 1000).toLocaleString() : 'Waiting for a fresh reading'}</p>
        <form className="thermostat-setting" onSubmit={event => { event.preventDefault(); void sendSetting('stale_minutes', stale) }}>
          <label>Fallback after (minutes)<input type="number" min="5" max="1440" step="1" required value={stale} onChange={event => setStale(event.target.value)} /></label>
          <button className="button" disabled={disabled || stale === String(t.stale_minutes)}>Save</button>
        </form>
        <p className="small">Uses the built-in sensor when the linked reading is missing, invalid or too old. Resumes automatically when fresh readings return.</p>
      </>}
      {settings.slice(3).map(control)}
      {smart && <p className="small">Disable smart control before changing the opening limit or control accuracy.</p>}
      {t.schedule_supported && <details><summary>Weekly schedule</summary><p className="small">The thermostat runs these daily temperature transitions. Limits follow its reported capabilities.{t.schedule_midnight_start && ' The first transition must start at midnight.'}</p>
        {days.map((_, day) => <ScheduleDay key={day} day={day} reported={t.schedule_details[day] ?? null} thermostat={t} disabled={disabled || t.pending.includes(`schedule_${day}`)} send={send} />)}
      </details>}
      <details><summary>Diagnostics</summary><dl>{[
        ...diagnostics,
        ['pi_heating_demand', 'Heating demand (%)'], ['pi_cooling_demand', 'Cooling demand (%)'], ['occupancy', 'Occupied'],
        ['control_sequence_of_operation', 'Control sequence'], ['feature_map', 'Features'],
      ].filter(([field]) => t.values[field] !== undefined).map(([field, label]) => <div key={field}><dt>{label}</dt><dd>{t.values[field] ?? 'Unknown'}</dd></div>)}</dl></details>
    </details>
  </div>
}

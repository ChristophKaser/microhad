import { useEffect, useId, useRef, useState } from 'react'
import { createPortal } from 'react-dom'
import type { Device, TemperatureSource, ThermostatSchedule, ThermostatState } from './api'
import { errorMessage, form, post } from './api'
import type { Setting } from './ThermostatPanel'

const days = ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday']
const week = [1, 2, 3, 4, 5, 6, 0]
const time = (minutes: number) => Number.isFinite(minutes) ? `${String(Math.floor(minutes / 60)).padStart(2, '0')}:${String(minutes % 60).padStart(2, '0')}` : ''
const sourceOf = (t: ThermostatState) => t.source_id || (t.values.temperature_sensor_select ? 'manual' : 'builtin')
const pendingField = (field: string) => field === 'temperature_source' ? 'temperature_sensor_select' : field
const sameSchedule = (a: ThermostatSchedule | null | undefined, b: ThermostatSchedule) => !!a && a.mode === b.mode && a.transitions.length === b.transitions.length && a.transitions.every((row, i) => row.time === b.transitions[i].time && row.heat === b.transitions[i].heat && row.cool === b.transitions[i].cool)

export function scheduleError(schedule: ThermostatSchedule, t: ThermostatState): string {
  if (![1, 2, 3].includes(schedule.mode) || (schedule.mode & 1 && !t.supported.occupied_heating_setpoint) || (schedule.mode & 2 && !t.supported.occupied_cooling_setpoint)) return 'Select a supported schedule type.'
  if (!schedule.transitions.length || schedule.transitions.length > Math.min(32, t.schedule_max_transitions)) return `Add between 1 and ${Math.min(32, t.schedule_max_transitions)} transitions.`
  if (t.schedule_midnight_start && schedule.transitions[0].time !== 0) return 'The first transition must start at 00:00.'
  let previous = -1
  for (const row of schedule.transitions) {
    if (!Number.isInteger(row.time) || row.time < 0 || row.time > 1439 || row.time <= previous) return 'Enter unique start times in increasing order.'
    previous = row.time
    for (const field of ['heat', 'cool'] as const) {
      if (!(schedule.mode & (field === 'heat' ? 1 : 2))) continue
      const value = row[field]
      const limits = t.fields[field === 'heat' ? 'occupied_heating_setpoint' : 'occupied_cooling_setpoint']
      if (value === undefined || !Number.isFinite(value) || (limits && (value < limits.min || value > limits.max || Math.abs(value / limits.step - Math.round(value / limits.step)) > .00001))) return `Enter a valid ${field === 'heat' ? 'heating' : 'cooling'} temperature within the device limits.`
    }
    if (schedule.mode === 3 && row.heat! + (t.values.min_setpoint_dead_band ?? 2.5) > row.cool!) return 'Cooling must exceed heating by the device’s minimum temperature gap.'
  }
  return ''
}

export function ThermostatDialog({ device, sources, settings, diagnostics, onClose }: {
  device: Device; sources: TemperatureSource[]; settings: Setting[]; diagnostics: string[][]; onClose: () => void
}) {
  const t = device.thermostat!
  const id = useId()
  const dialog = useRef<HTMLDialogElement>(null)
  const [tab, setTab] = useState('settings')
  const [day, setDay] = useState(1)
  const [values, setValues] = useState<Record<string, string>>({})
  const [drafts, setDrafts] = useState<Record<number, ThermostatSchedule>>({})
  const [submitted, setSubmitted] = useState<Record<string, string>>({})
  const [results, setResults] = useState<Record<string, string>>({})
  const [errors, setErrors] = useState<Record<string, string>>({})
  const [copy, setCopy] = useState(false)
  const [destinations, setDestinations] = useState<number[]>([])
  const [replace, setReplace] = useState(false)
  const [discard, setDiscard] = useState(false)
  const [busy, setBusy] = useState(false)
  const dirty = Object.keys(values).length + Object.keys(drafts).length > 0
  const disabled = busy || device.controller_available === false
  const latest = useRef(t)
  const controllerAvailable = useRef(device.controller_available !== false)
  const sending = useRef(false)
  const pendingSeen = useRef(new Set<string>())
  const submissionErrors = useRef<Record<string, string>>({})
  const readPending = useRef(false)
  const [reading, setReading] = useState<number | null>(null)
  latest.current = t
  controllerAvailable.current = device.controller_available !== false
  useEffect(() => {
    const previous = document.activeElement as HTMLElement | null
    dialog.current?.showModal()
    return () => { previous?.focus() }
  }, [])
  useEffect(() => { if (discard) dialog.current?.querySelector('.thermostat-discard')?.scrollIntoView({ block: 'nearest' }) }, [discard])
  function reported(field: string, state = t) {
    return field === 'temperature_source' ? sourceOf(state) : field === 'stale_minutes' ? String(state.stale_minutes) : String(state.values[field] ?? '')
  }
  useEffect(() => {
    const confirmed: string[] = [], failed: [string, string][] = []
    for (const [field, value] of Object.entries(submitted)) {
      if (t.pending.includes(pendingField(field))) { pendingSeen.current.add(field); continue }
      const matches = field.startsWith('schedule_') ? sameSchedule(t.schedule_details[Number(field.slice(9))], JSON.parse(value)) : reported(field) === value
      if (matches) confirmed.push(field)
      else if (pendingSeen.current.has(field) || (t.error && t.error !== submissionErrors.current[field])) failed.push([field, value])
    }
    if (!confirmed.length && !failed.length) return
    for (const field of [...confirmed, ...failed.map(([field]) => field)]) pendingSeen.current.delete(field)
    setSubmitted(current => {
      const next = { ...current }
      for (const field of [...confirmed, ...failed.map(([field]) => field)]) delete next[field]
      return next
    })
    setResults(current => { const next = { ...current }; for (const field of confirmed) next[field] = 'Confirmed'; for (const [field] of failed) next[field] = ''; return next })
    for (const [field, value] of failed) {
      if (field.startsWith('schedule_')) setDrafts(current => ({ ...current, [Number(field.slice(9))]: JSON.parse(value) }))
      else setValues(current => ({ ...current, [field]: value }))
      setErrors(current => ({ ...current, [field]: t.error || 'The device did not confirm this change. Review and retry.' }))
    }
  }, [t, submitted])
  useEffect(() => {
    if (reading === null) return
    const field = `schedule_${reading}`
    if (t.pending.includes(field)) readPending.current = true
    else if (readPending.current) {
      readPending.current = false; setReading(null)
      setResults(current => ({ ...current, [field]: t.error ? '' : 'Read complete' }))
      if (t.error) setErrors(current => ({ ...current, [field]: t.error }))
    }
  }, [t, reading])
  function unavailable(field: string) {
    return disabled || t.pending.includes(pendingField(field)) || submitted[field] !== undefined ||
      (field === 'external_temperature_input' && !!t.source_id) ||
      (['valve_opening_degree', 'temperature_accuracy'].includes(field) && t.values.smart_temperature_control === 2)
  }
  function edit(field: string, value: string) {
    setValues(old => { const next = { ...old, [field]: value }; if (value === reported(field)) delete next[field]; return next })
    setErrors(old => ({ ...old, [field]: '' }))
    setResults(old => ({ ...old, [field]: '' }))
  }
  function close() { if (!busy) { if (dirty) setDiscard(true); else onClose() } }
  function fieldStatus(field: string) {
    return <>{errors[field] && <span className="error" role="alert">{errors[field]}</span>}<small role="status">{submitted[field] !== undefined || t.pending.includes(pendingField(field)) ? 'Queued · awaiting device confirmation' : results[field]}</small></>
  }
  function adjusted(setting: Setting) {
    const result = { ...setting, ...t.fields[setting.field] }
    if (setting.field === 'temporary_mode_duration' && Number(values.temporary_mode_select ?? t.values.temporary_mode_select) !== 1) result.max = 180
    return result
  }
  function settingControl(original: Setting) {
    if (!t.supported[original.field]) return null
    const setting = adjusted(original), field = setting.field
    return <label className="thermostat-dialog-field" key={field}>{setting.label}{setting.unit && ` (${setting.unit})`}
      {setting.options ? <select value={values[field] ?? submitted[field] ?? reported(field)} disabled={unavailable(field) || t.fields[field]?.writable === false} onChange={e => edit(field, e.target.value)}>
        <option value="" disabled>Unknown</option>{setting.options.map(([value, label]) => <option key={value} value={value} disabled={field === 'temporary_mode_select' && value === 255}>{label}</option>)}
      </select> : <input type="number" value={values[field] ?? submitted[field] ?? reported(field)} placeholder="Unknown" min={setting.min} max={setting.max} step={setting.step ?? 'any'} disabled={unavailable(field) || t.fields[field]?.writable === false} onChange={e => edit(field, e.target.value)} />}
      {fieldStatus(field)}
    </label>
  }
  const schedule = drafts[day] ?? (submitted[`schedule_${day}`] ? JSON.parse(submitted[`schedule_${day}`]) as ThermostatSchedule : t.schedule_details[day]) ?? { mode: t.supported.occupied_heating_setpoint ? 1 : 2, transitions: [] }
  const dayDisabled = unavailable(`schedule_${day}`)
  function editDay(next: ThermostatSchedule) {
    setDrafts(old => ({ ...old, [day]: next })); setErrors(old => ({ ...old, [`schedule_${day}`]: '' })); setResults(old => ({ ...old, [`schedule_${day}`]: '' }))
  }
  function editRow(index: number, field: 'time' | 'heat' | 'cool', value: number) {
    editDay({ ...schedule, transitions: schedule.transitions.map((row, i) => i === index ? { ...row, [field]: value } : row) })
  }
  async function send(field: string, value: string) {
    await post(`/api/devices/${encodeURIComponent(device.id)}/thermostat`, form({ field, value }))
  }
  async function save() {
    if (disabled || sending.current) return
    const validation: Record<string, string> = {}
    for (const [field, value] of Object.entries(values)) {
      const definition = settings.find(s => s.field === field)
      const metadata = definition ? adjusted(definition) : field === 'stale_minutes' ? { min: 5, max: 1440, step: 1 } : undefined
      const number = Number(value)
      if (field !== 'temperature_source' && (!value || !Number.isFinite(number) || (metadata && ((metadata.min !== undefined && number < metadata.min) || (metadata.max !== undefined && number > metadata.max) || (metadata.step && Math.abs(number / metadata.step - Math.round(number / metadata.step)) > .00001))) || (definition?.options && !definition.options.some(([option]) => String(option) === value)))) validation[field] = 'Enter a valid value within the shown device limits.'
      if (unavailable(field) || t.fields[field]?.writable === false) validation[field] = 'Wait for device confirmation or resolve the control dependency before saving.'
    }
    for (const [key, draft] of Object.entries(drafts)) {
      const field = `schedule_${key}`, error = scheduleError(draft, t)
      if (error || unavailable(field)) validation[field] = error || 'Wait for this day’s pending request before saving.'
    }
    setErrors(validation)
    if (Object.keys(validation).length) { setTab(Object.keys(validation)[0].startsWith('schedule_') ? 'schedule' : 'settings'); return }
    sending.current = true; setBusy(true)
    // Settings first, with source and temporary mode preceding dependent fields.
    const entries = [...Object.entries(values).sort(([a], [b]) => (a === 'temperature_source' || a === 'temporary_mode_select' ? -1 : 0) - (b === 'temperature_source' || b === 'temporary_mode_select' ? -1 : 0)), ...Object.entries(drafts).map(([key, draft]) => [`schedule_${key}`, JSON.stringify(draft)])]
    try {
      for (const [field, value] of entries) {
        if (!controllerAvailable.current) { setErrors(old => ({ ...old, [field]: 'Controller unavailable. Retry when connected.' })); continue }
        if (latest.current.pending.includes(pendingField(field))) {
          setErrors(old => ({ ...old, [field]: 'A request is already pending. Retry after confirmation.' })); continue
        }
        try {
          await send(field, value)
          submissionErrors.current[field] = latest.current.error
          pendingSeen.current.delete(field)
          setSubmitted(old => ({ ...old, [field]: value }))
          setResults(old => ({ ...old, [field]: field === 'stale_minutes' ? 'Saved' : 'Queued · awaiting device confirmation' }))
          if (field.startsWith('schedule_')) setDrafts(old => { const next = { ...old }; delete next[Number(field.slice(9))]; return next })
          else setValues(old => { const next = { ...old }; delete next[field]; return next })
        } catch (reason) { setErrors(old => ({ ...old, [field]: errorMessage(reason) })) }
      }
    } finally { sending.current = false; setBusy(false) }
  }
  const groups = [
    { name: 'Comfort and protection', fields: ['unoccupied_heating_setpoint', 'unoccupied_cooling_setpoint', 'local_temperature_calibration', 'child_lock', 'open_window', 'frost_protection_temperature', 'temporary_mode_select', 'temporary_mode_duration', 'timer_mode_target_temp'] },
    { name: 'Advanced controls', fields: settings.map(s => s.field).filter(field => !['unoccupied_heating_setpoint', 'unoccupied_cooling_setpoint', 'local_temperature_calibration', 'child_lock', 'open_window', 'frost_protection_temperature', 'temporary_mode_select', 'temporary_mode_duration', 'timer_mode_target_temp', 'external_temperature_input'].includes(field)) },
  ]
  return createPortal(<dialog ref={dialog} className="dialog wide thermostat-dialog" aria-labelledby={`${id}-title`} onCancel={e => { e.preventDefault(); close() }}>
    <div className="dialog-heading"><div><h2 id={`${id}-title`}>Thermostat settings</h2><p>{device.name || device.id}</p></div><button type="button" className="icon-button" onClick={close} disabled={busy} aria-label="Close">✕</button></div>
    <div className="thermostat-tabs" role="tablist" aria-label="Thermostat sections" onKeyDown={event => {
      if (!t.schedule_supported || !['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(event.key)) return
      event.preventDefault()
      const next = event.key === 'Home' ? 'settings' : event.key === 'End' ? 'schedule' : tab === 'settings' ? 'schedule' : 'settings'
      setTab(next); document.getElementById(`${id}-${next}-tab`)?.focus()
    }}>
      <button type="button" className="button" role="tab" id={`${id}-settings-tab`} aria-controls={`${id}-settings-panel`} aria-selected={tab === 'settings'} tabIndex={tab === 'settings' ? 0 : -1} onClick={() => setTab('settings')}>Settings</button>
      {t.schedule_supported && <button type="button" className="button" role="tab" id={`${id}-schedule-tab`} aria-controls={`${id}-schedule-panel`} aria-selected={tab === 'schedule'} tabIndex={tab === 'schedule' ? 0 : -1} onClick={() => setTab('schedule')}>Weekly schedule</button>}
    </div>
    {device.controller_available === false && <p role="status">Controller unavailable. Changes cannot be sent.</p>}
    {t.error && <p className="error" role="alert">{t.error}</p>}
    {Object.values(errors).some(Boolean) && <p className="error" role="alert">Some changes need attention. Check the marked settings or days.</p>}
    <div role="tabpanel" id={`${id}-${tab}-panel`} aria-labelledby={`${id}-${tab}-tab`}>
    {tab === 'settings' ? <>
      {t.external_input_supported && <section><h3>Temperature source</h3>
        <label className="thermostat-dialog-field">Temperature source<select value={values.temperature_source ?? submitted.temperature_source ?? sourceOf(t)} disabled={unavailable('temperature_source') || !t.supported.temperature_sensor_select} onChange={e => edit('temperature_source', e.target.value)}>
          <option value="builtin">Built-in sensor</option><option value="manual">Manual / API input</option>
          {t.source_id && !sources.some(s => s.id === t.source_id) && <option value={t.source_id}>Missing sensor ({t.source_id})</option>}
          {sources.filter(s => s.id !== device.id).map(s => <option key={s.id} value={s.id}>{s.name || s.id}</option>)}
        </select>{fieldStatus('temperature_source')}</label>
        {t.source_id && <p className="small">Linked sensor: {sources.find(s => s.id === t.source_id)?.name || t.source_id}<br />Last reading: {t.source_observed_at ? new Date(t.source_observed_at * 1000).toLocaleString() : 'Waiting for a fresh reading'}</p>}
        {(t.source_id || !['builtin', 'manual'].includes(values.temperature_source ?? submitted.temperature_source ?? sourceOf(t))) && <label className="thermostat-dialog-field">Fallback after (minutes)<input type="number" min="5" max="1440" step="1" value={values.stale_minutes ?? submitted.stale_minutes ?? String(t.stale_minutes)} disabled={unavailable('stale_minutes')} onChange={e => edit('stale_minutes', e.target.value)} />{fieldStatus('stale_minutes')}<small>Uses the built-in sensor when linked readings are missing, invalid or too old. Fresh readings restore the linked source.</small></label>}
        {settings.filter(s => s.field === 'external_temperature_input').map(settingControl)}
      </section>}
      {!t.external_input_supported && settings.filter(s => s.field === 'external_temperature_input').map(settingControl)}
      {groups.map(group => group.fields.some(field => t.supported[field]) && <section key={group.name}><h3>{group.name}</h3><div className="thermostat-fields">{settings.filter(s => group.fields.includes(s.field)).map(settingControl)}</div></section>)}
      {t.values.smart_temperature_control === 2 && <p className="small">Disable smart control and wait for confirmation before changing the opening limit or control accuracy.</p>}
      <details><summary>Diagnostics</summary><dl>{[...diagnostics, ['pi_heating_demand', 'Heating demand (%)'], ['pi_cooling_demand', 'Cooling demand (%)'], ['occupancy', 'Occupied'], ['control_sequence_of_operation', 'Control sequence'], ['feature_map', 'Features']].filter(([field]) => t.values[field] !== undefined).map(([field, label]) => <div key={field}><dt>{label}</dt><dd>{t.values[field]}</dd></div>)}</dl></details>
    </> : <section>
      <p className="small">Each temperature stays active until the next transition. {t.modes.includes(1) ? 'Select Auto on the device card to run the schedule.' : 'Schedules require a device mode that supports automatic operation.'} Saving does not change operating mode.</p>
      <div className="thermostat-week">{week.map(index => {
        const field = `schedule_${index}`, entry = drafts[index] ?? (submitted[field] ? JSON.parse(submitted[field]) as ThermostatSchedule : t.schedule_details[index])
        return <button type="button" className="button thermostat-week-day" key={index} aria-pressed={day === index} onClick={() => { setDay(index); setCopy(false); setDestinations([]); setReplace(false) }}><strong>{days[index]}</strong><small>{entry?.transitions.map(row => `${time(row.time)} ${[row.heat !== undefined && `${row.heat}° heat`, row.cool !== undefined && `${row.cool}° cool`].filter(Boolean).join(' / ')}`).join(' · ') || (entry ? 'No transitions' : 'Not read yet')}</small><small>{errors[field] ? 'Needs attention' : drafts[index] ? 'Unsaved changes' : submitted[field] || t.pending.includes(field) ? 'Pending confirmation' : results[field] || (entry ? 'Reported' : 'Unread')}</small></button>
      })}</div>
      <h3>{days[day]}</h3>
      {fieldStatus(`schedule_${day}`)}
      <fieldset disabled={dayDisabled} className="thermostat-day-editor"><legend>Daily transitions</legend>
        <label className="thermostat-dialog-field">Schedule type<select value={schedule.mode} onChange={e => {
          const mode = Number(e.target.value)
          editDay({ mode, transitions: schedule.transitions.map(row => ({ time: row.time, ...(mode & 1 ? { heat: row.heat ?? 20 } : {}), ...(mode & 2 ? { cool: row.cool ?? 25 } : {}) })) })
        }}>{[[1, 'Heating'], [2, 'Cooling'], [3, 'Heating and cooling']].filter(([mode]) => (!(Number(mode) & 1) || t.supported.occupied_heating_setpoint) && (!(Number(mode) & 2) || t.supported.occupied_cooling_setpoint)).map(([mode, label]) => <option key={mode} value={mode}>{label}</option>)}</select></label>
        <p className="small">Up to {Math.min(32, t.schedule_max_transitions)} transitions.{t.schedule_midnight_start && ' The first must start at 00:00.'}</p>
        {schedule.transitions.map((row, index) => <div className="thermostat-transition" key={index}>
          <label>Start time<input type="time" value={time(row.time)} onChange={e => { const parts = e.target.value.split(':').map(Number); editRow(index, 'time', parts.length === 2 ? parts[0] * 60 + parts[1] : NaN) }} /></label>
          {(['heat', 'cool'] as const).filter(field => schedule.mode & (field === 'heat' ? 1 : 2)).map(field => { const limits = t.fields[field === 'heat' ? 'occupied_heating_setpoint' : 'occupied_cooling_setpoint']; return <label key={field}>{field === 'heat' ? 'Heat' : 'Cool'} (°C)<input type="number" min={limits?.min} max={limits?.max} step={limits?.step ?? .01} value={Number.isFinite(row[field]) ? row[field] : ''} onChange={e => editRow(index, field, e.target.value === '' ? NaN : Number(e.target.value))} /></label> })}
          <button type="button" className="button" aria-label={`Remove ${days[day]} transition ${index + 1}`} onClick={() => editDay({ ...schedule, transitions: schedule.transitions.filter((_, i) => i !== index) })}>Remove</button>
        </div>)}
        <button type="button" className="button" disabled={schedule.transitions.length >= Math.min(32, t.schedule_max_transitions) || schedule.transitions[schedule.transitions.length - 1]?.time === 1439} onClick={() => editDay({ ...schedule, transitions: [...schedule.transitions, { time: schedule.transitions.length ? Math.min(1439, schedule.transitions[schedule.transitions.length - 1].time + 60) : 0, ...(schedule.mode & 1 ? { heat: 20 } : {}), ...(schedule.mode & 2 ? { cool: 25 } : {}) }] })}>Add transition</button>
      </fieldset>
      <div className="thermostat-tabs"><button type="button" className="button" disabled={disabled || t.pending.includes(`schedule_${day}`) || !!drafts[day]} onClick={async () => {
        if (sending.current) return
        sending.current = true; setBusy(true); setErrors(old => ({ ...old, [`schedule_${day}`]: '' }))
        try { await send(`schedule_${day}`, '__read__'); readPending.current = false; setReading(day); setResults(old => ({ ...old, [`schedule_${day}`]: 'Read requested · awaiting device response' })) } catch (reason) { setErrors(old => ({ ...old, [`schedule_${day}`]: errorMessage(reason) })) } finally { sending.current = false; setBusy(false) }
      }}>Read from device</button><button type="button" className="button" disabled={dayDisabled} onClick={() => { setCopy(!copy); setReplace(false) }}>Copy to…</button></div>
      {drafts[day] && <p className="small">Save or discard edits before reading this day from the device.</p>}
      {copy && <fieldset disabled={disabled}><legend>Copy {days[day]} to</legend><div className="thermostat-copy-days">{week.filter(index => index !== day).map(index => <label key={index}><input type="checkbox" checked={destinations.includes(index)} disabled={unavailable(`schedule_${index}`)} onChange={e => { setDestinations(old => e.target.checked ? [...old, index] : old.filter(d => d !== index)); setReplace(false) }} />{days[index]}</label>)}</div>
        {replace && <p role="alert">This will replace unsaved edits in the selected days.</p>}
        <button type="button" className="button" disabled={!destinations.length || destinations.some(index => unavailable(`schedule_${index}`))} onClick={() => {
          const error = scheduleError(schedule, t); if (error) { setErrors(old => ({ ...old, [`schedule_${day}`]: error })); return }
          if (!replace && destinations.some(index => drafts[index])) { setReplace(true); return }
          setDrafts(old => { const next = { ...old }; for (const index of destinations) next[index] = { mode: schedule.mode, transitions: schedule.transitions.map(row => ({ ...row })) }; return next }); setCopy(false); setDestinations([]); setReplace(false)
        }}>{replace ? 'Replace drafts' : 'Stage copy'}</button><p className="small">Copied days are applied only when you save changes.</p>
      </fieldset>}
    </section>}
    </div>
    <div className="dialog-actions"><span className="small" role="status">{busy ? 'Sending requests…' : dirty ? 'Unsaved changes' : Object.keys(submitted).length ? 'Requests queued. Awaiting device confirmation.' : 'No unsaved changes'}</span><button type="button" className="button" disabled={!dirty || disabled} onClick={() => void save()}>Save changes</button><button type="button" className="button" disabled={busy} onClick={close}>Close</button></div>
    {discard && <div className="thermostat-discard" role="alert"><p>Discard unsaved changes?</p><button type="button" className="button" autoFocus onClick={() => setDiscard(false)}>Keep editing</button> <button type="button" className="button" onClick={onClose}>Discard changes</button></div>}
  </dialog>, document.body)
}

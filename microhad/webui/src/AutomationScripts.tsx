import { useCallback, useEffect, useRef, useState } from 'react'
import { errorMessage, type Device, type SceneGroupSummary } from './api'
import { useAutomationEditor } from './useAutomationEditor'
import { automationAction, disabledFilename, listAutomations, readAutomation, readAutomationLogs, validAutomationFilename, visibleAutomationLogs, type AutomationLog, type AutomationScript } from './automation'

export function AutomationScripts({ onDirtyChange, devices = [], groups = [] }: { onDirtyChange: (dirty: boolean) => void; devices?: Device[]; groups?: SceneGroupSummary[] }) {
  const [scripts, setScripts] = useState<AutomationScript[]>([])
  const [records, setRecords] = useState<AutomationLog[]>([])
  const [selected, setSelected] = useState('')
  const [source, setSource] = useState('')
  const [saved, setSaved] = useState('')
  const [exists, setExists] = useState(false)
  const [message, setMessage] = useState('')
  const [error, setError] = useState('')
  const [busy, setBusy] = useState(false)
  const [severity, setSeverity] = useState('')
  const [since, setSince] = useState('')
  const [cleared, setCleared] = useState(0)
  const editor = useRef<HTMLTextAreaElement>(null)
  const aceEditor = useAutomationEditor(selected, source, setSource, { devices, groups }, editor)
  const dirty = selected !== '' && (!exists || source !== saved)
  const script = scripts.find(item => item.filename === selected)
  const scriptId = selected.replace(/\.lua(?:\.disabled)?$/, '')
  const refresh = useCallback(async () => {
    const [list, log] = await Promise.all([listAutomations(), readAutomationLogs()])
    setScripts(list.scripts); setRecords(log.records)
  }, [])
  const refreshDirectory = async () => {
    setBusy(true); setError('')
    try { await automationAction('refresh', {}); const list = await listAutomations(); setScripts(list.scripts) }
    catch (failure) { setError(errorMessage(failure)) }
    finally { setBusy(false) }
  }
  useEffect(() => {
    let active = true
    const poll = async () => { try { if (active) await refresh() } catch (failure) { if (active) setError(errorMessage(failure)) } }
    void poll(); const timer = window.setInterval(() => void poll(), 2000)
    return () => { active = false; window.clearInterval(timer) }
  }, [refresh])
  useEffect(() => { onDirtyChange(dirty); return () => onDirtyChange(false) }, [dirty, onDirtyChange])
  useEffect(() => {
    if (!dirty) return
    const leave = (event: BeforeUnloadEvent) => { event.preventDefault(); event.returnValue = '' }
    window.addEventListener('beforeunload', leave)
    return () => window.removeEventListener('beforeunload', leave)
  }, [dirty])
  const discard = () => !dirty || window.confirm('Discard unsaved script changes?')
  const choose = async (name: string) => {
    if (!discard()) return
    setBusy(true)
    try { const value = await readAutomation(name); setSelected(name); setSource(value.source); setSaved(value.source); setExists(true); setMessage(''); setError('') }
    catch (failure) { setError(errorMessage(failure)) }
    finally { setBusy(false) }
  }
  const action = async (name: string, fields: Record<string, string> = {}) => {
    setBusy(true); setError(''); setMessage('')
    try {
      if (name === 'save') await automationAction('validate', { filename: fields.filename, source: fields.source })
      await automationAction(name, fields)
      if (name === 'save') { setSaved(fields.source); setExists(true) }
      if (name === 'delete') { setSelected(''); setSource(''); setSaved(''); setExists(false) }
      if (name === 'enable' || name === 'disable') setSelected(disabledFilename(selected, name === 'enable'))
      setMessage(name === 'validate' ? 'Validation passed' : name === 'save' ? 'Source saved' : 'Request completed')
      await refresh()
    } catch (failure) { setError(errorMessage(failure)) }
    finally { setBusy(false) }
  }
  const create = () => {
    if (!discard()) return
    const entered = window.prompt('Script name')?.trim()
    if (!entered) return
    const name = entered.includes('.') ? entered : `${entered}.lua`
    if (!validAutomationFilename(name)) { setError('Use letters, digits, _ or - followed by .lua.'); return }
    if (scripts.some(item => item.id === name.replace(/\.lua(?:\.disabled)?$/, ''))) { setError('A script with that ID already exists.'); return }
    setSelected(name); setSource(`-- ${name}\n\n`); setSaved(''); setExists(false); setMessage('New script — save to create the source file.'); setError('')
  }
  const save = (reload: boolean) => { if (selected) void action('save', { filename: selected, source, reload: reload ? '1' : '0' }) }
  useEffect(() => {
    const keyboard = (event: KeyboardEvent) => { if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 's') { event.preventDefault(); if (!busy) save(false) } }
    window.addEventListener('keydown', keyboard); return () => window.removeEventListener('keydown', keyboard)
  })
  const jumpToLine = (line?: number) => {
    if (line) aceEditor.jump(line)
  }
  const logs = visibleAutomationLogs(records, scriptId, severity, since ? new Date(since).getTime() / 1000 : 0, cleared)
  const timestamp = (epoch?: number) => epoch ? new Date(epoch * 1000).toLocaleString() : '—'
  return <section className="automation-page">
    <div className="panel-heading"><h2>Automation scripts</h2></div>
    <div className="automation-layout"><aside className="automation-list">
      <div className="automation-directory-actions">
        <button className="button" disabled={busy} onClick={() => void refreshDirectory()}>Refresh</button>
        <button className="button primary" disabled={busy} onClick={create}>New script</button>
      </div>
      {scripts.map(item => <button key={item.id} disabled={busy} className={`automation-script ${selected === item.filename ? 'selected' : ''}`} onClick={() => void choose(item.filename)}><strong>{item.id}</strong><small>{item.enabled ? item.loaded ? `Generation ${item.generation}` : 'Load failed' : 'Disabled'}</small>{item.error && <small className="error">{item.error}</small>}</button>)}
    </aside><div className="automation-editor">{selected ? <>
      <div className="automation-toolbar"><strong>{selected}</strong>{dirty && <span>Unsaved changes</span>}</div>
      <div className="automation-actions">
        <button className="button" disabled={busy || !exists} onClick={() => void action('reload', { filename: selected })}>Reload</button>
        <button className="button" disabled={busy || !exists} onClick={() => void action(script?.enabled ? 'disable' : 'enable', { filename: selected })}>{script?.enabled ? 'Disable' : 'Enable'}</button>
        <button className="button" disabled={busy || !exists} onClick={() => { if (window.confirm('Reset this script’s persistent state?')) void action('reset', { filename: selected }) }}>Reset persistent state</button>
        <button className="button danger" disabled={busy || !exists} onClick={() => { if (window.confirm('Delete this script source? Persistent state will be retained.')) void action('delete', { filename: selected }) }}>Delete</button>
      </div>
      {script && <div aria-label="Script status"><p>{script.enabled ? script.loaded ? 'Loaded' : 'Load failed' : 'Disabled'} · generation {script.generation} · {script.listeners} listeners · {script.timers} timers/cron · {script.coroutines} suspended callbacks</p><p>Reload: {timestamp(script.loaded_at)} · Validated: {timestamp(script.validated_at)} · Last event: {timestamp(script.last_event_at)} · Last callback: {timestamp(script.last_callback_at)}</p>{script.error && <p className="form-error">{script.error}</p>}</div>}
      <div className="automation-toolbar">
        <button className="button primary" disabled={busy || !dirty} onClick={() => save(true)}>Save and reload</button>
        {error && <p role="alert" className="automation-error"><strong>Action failed</strong><br />{error}</p>}
      </div>
      <div ref={aceEditor.container} className="automation-ace" hidden={!aceEditor.ready} />
      {aceEditor.warning && <p role="status">{aceEditor.warning}</p>}
      <textarea hidden={aceEditor.ready} ref={editor} className="automation-source" aria-label="Lua source" value={source} onChange={event => setSource(event.target.value)} spellCheck={false} />
      <details open className="automation-log-section"><summary>Logs and status</summary><div className="automation-toolbar">
        <label>Severity <select value={severity} onChange={event => setSeverity(event.target.value)}><option value="">All</option>{['debug', 'info', 'warn', 'error'].map(level => <option key={level}>{level}</option>)}</select></label>
        <label>Since <input type="datetime-local" value={since} onChange={event => setSince(event.target.value)} /></label>
        <button className="button" onClick={() => setCleared(Math.max(0, ...records.map(row => row.record_id)))}>Clear displayed logs</button>
      </div><div className="automation-logs">{logs.length === 0 && <p className="automation-log-empty">No log entries match the current filters.</p>}{logs.map(row => <div key={row.record_id} className={`automation-log automation-log-${row.level}`}>
        <div className="automation-log-meta"><time className="log-time">{timestamp(row.at)}</time><strong className="automation-log-level">{row.level}</strong><span className="automation-log-id">#{row.record_id}</span><button className="automation-log-copy" onClick={() => void navigator.clipboard.writeText(JSON.stringify(row, null, 2)).catch(failure => setError(errorMessage(failure)))}>Copy record</button></div>
        {row.line ? <button className="automation-log-message" onClick={() => jumpToLine(row.line)}><span className="automation-log-location">Line {row.line}: </span>{row.message}</button> : <div className="automation-log-message">{row.message}</div>}
        {Object.keys(row.fields).length > 0 && <pre>{JSON.stringify(row.fields, null, 2)}</pre>}
      </div>)}</div></details>
    </> : <p className="muted">Select a script or create one. Subscribe with <code>events.on</code>; unsubscribe with <code>events.off</code>.</p>}
    {message && <p role="status">{message}</p>}{error && !selected && <p role="alert" className="automation-error"><strong>Action failed</strong><br />{error}</p>}
    </div></div>
  </section>
}

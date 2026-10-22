import { useEffect, useState } from 'react'
import { errorMessage, form, jsonRequest, post } from './api'

type Script = { filename: string; id: string; enabled: boolean; generation: number; loaded_at: number; error: string }

export function AutomationScripts() {
  const [scripts, setScripts] = useState<Script[]>([])
  const [selected, setSelected] = useState('')
  const [source, setSource] = useState('')
  const [saved, setSaved] = useState('')
  const [message, setMessage] = useState('')
  const dirty = selected !== '' && source !== saved
  const loadList = () => jsonRequest<{ scripts: Script[] }>('/api/automations').then(value => {
    setScripts(value.scripts)
    if (selected && !value.scripts.some(script => script.filename === selected)) { setSelected(''); setSource(''); setSaved('') }
  }).catch(error => setMessage(errorMessage(error)))
  useEffect(() => { void loadList() }, [])
  const choose = async (filename: string) => {
    if (dirty && !window.confirm('Discard unsaved script changes?')) return
    try {
      const value = await jsonRequest<{ source: string }>(`/api/automations/${encodeURIComponent(filename)}`)
      setSelected(filename); setSource(value.source); setSaved(value.source); setMessage('')
    } catch (error) { setMessage(errorMessage(error)) }
  }
  const action = async (name: string, values: Record<string, string> = {}) => {
    try { await post(`/api/automations/${name}`, form(values)); setMessage(name === 'validate' ? 'Validation passed' : 'Saved'); await loadList() }
    catch (error) { setMessage(errorMessage(error)) }
  }
  const create = () => {
    const filename = window.prompt('Script filename (ending in .lua)')
    if (!filename) return
    setSelected(filename); setSource('-- ' + filename + '\n\n'); setSaved(''); setMessage('New script — save it to create the source file.')
  }
  const selectedScript = scripts.find(script => script.filename === selected)
  return <section className="automation-page">
    <div className="panel-heading"><div><p className="eyebrow">Gateway-local Lua</p><h2>Automation scripts</h2></div><button className="button primary" onClick={create}>New script</button></div>
    <div className="automation-layout">
      <aside className="automation-list">
        <button className="button" onClick={() => void action('reload', { filename: 'all' })}>Reload all</button>
        {scripts.map(script => <button key={script.filename} className={`automation-script ${selected === script.filename ? 'selected' : ''}`} onClick={() => void choose(script.filename)}>
          <strong>{script.id}</strong><small>{script.enabled ? `Generation ${script.generation}` : 'Disabled'}</small>{script.error && <small className="error">{script.error}</small>}
        </button>)}
      </aside>
      <div className="automation-editor">
        {selected ? <>
          <div className="automation-toolbar"><strong>{selected}</strong><span>{dirty ? 'Unsaved changes' : 'Saved'}</span>
            <button className="button" onClick={() => void action('validate', { filename: selected, source })}>Validate</button>
            <button className="button" onClick={() => void action('save', { filename: selected, source, reload: '0' })}>Save</button>
            <button className="button primary" onClick={() => void action('save', { filename: selected, source })}>Save and reload</button>
            <button className="button" onClick={() => void action('reload', { filename: selected })}>Reload</button>
          </div>
          <textarea className="automation-source" aria-label="Lua source" value={source} onChange={event => setSource(event.target.value)} spellCheck={false} />
          <div className="automation-actions">
            <button className="button" onClick={() => void action(selectedScript?.enabled ? 'disable' : 'enable', { filename: selected })}>{selectedScript?.enabled ? 'Disable' : 'Enable'}</button>
            <button className="button" onClick={() => { if (window.confirm('Reset this script’s persistent state?')) void action('reset', { filename: selected }) }}>Reset persistent state</button>
            <button className="button danger" onClick={() => { if (window.confirm('Delete this script source?')) void action('delete', { filename: selected }) }}>Delete</button>
          </div>
        </> : <p className="muted">Select a script or create one. Scripts use <code>events.on</code> and <code>events.off</code>.</p>}
        {message && <p className="form-error">{message}</p>}
      </div>
    </div>
  </section>
}

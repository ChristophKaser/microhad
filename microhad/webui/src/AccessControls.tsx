import { useEffect, useState } from 'react'
import { accessStatus, login, logout, roles, sessionToken, setPassword } from './auth'

export function AccessControls() {
  const [open, setOpen] = useState(false)
  const [access, setAccess] = useState(-1)
  const [configured, setConfigured] = useState<boolean[]>([])
  const [role, setRole] = useState<string>('administrator')
  const [password, setValue] = useState('')
  const [confirmation, setConfirmation] = useState('')
  const [remember, setRemember] = useState(false)
  const [editing, setEditing] = useState(false)
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState('')
  const refresh = () => { void accessStatus().then(value => { setAccess(value.role); setConfigured(value.passwords) }).catch(() => {}) }
  useEffect(() => { refresh(); const required = () => setOpen(true); window.addEventListener('microhad-login-required', required); return () => window.removeEventListener('microhad-login-required', required) }, [])
  async function submit() {
    setBusy(true); setError('')
    try {
      if (editing) { if (password !== confirmation) throw new Error('Passwords do not match'); await setPassword(role, password) }
      else await login(role, password, remember)
      setValue(''); setConfirmation(''); setOpen(false); setEditing(false); location.reload()
    } catch (e) { setError(e instanceof Error ? e.message : 'Unable to update access') }
    finally { setBusy(false) }
  }
  return <>
    <span className="muted small">{access < 0 ? 'Login required' : roles[access]}</span>
    <button className="button" onClick={() => { setEditing(false); setOpen(true) }}>Log in</button>
    {sessionToken() && <button className="button" onClick={() => void logout().then(() => location.reload()).catch(e => setError(String(e)))}>Log out</button>}
    {access === 2 && <button className="button" onClick={() => { setEditing(true); setOpen(true) }}>Access passwords</button>}
    {open && <div className="modal"><section className="dialog" role="dialog" aria-modal="true" aria-label={editing ? 'Access passwords' : 'Log in'}>
      <h2>{editing ? 'Access passwords' : 'Log in'}</h2>
      <form onSubmit={event => { event.preventDefault(); void submit() }}>
        <label className="field">Access level<select value={role} onChange={event => setRole(event.target.value)}>{roles.map((name, i) => <option key={name} value={name}>{name}{configured[i] ? ' (password set)' : ''}</option>)}</select></label>
        <label className="field">{editing ? 'New password (empty removes it)' : 'Password'}<input type="password" autoFocus autoComplete={editing ? 'new-password' : 'current-password'} value={password} onChange={event => setValue(event.target.value)} /></label>
        {editing ? <><label className="field">Confirm password<input type="password" autoComplete="new-password" value={confirmation} onChange={event => setConfirmation(event.target.value)} /></label><p className="muted">Empty roles inherit the password below them. Changes log out all sessions.</p></> : <label><input type="checkbox" checked={remember} onChange={event => setRemember(event.target.checked)} /> Remember me</label>}
        {error && <p role="alert">{error}</p>}
        <div className="toolbar"><button className="button primary" disabled={busy} type="submit">{busy ? 'Please wait…' : editing ? 'Save password' : 'Log in'}</button><button className="button" type="button" disabled={busy} onClick={() => { setOpen(false); setValue(''); setConfirmation(''); setError('') }}>Cancel</button></div>
      </form>
    </section></div>}
  </>
}

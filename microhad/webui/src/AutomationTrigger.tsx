import { useRef, useState } from 'react'
import { errorMessage } from './api'
import { automationAction } from './automation'

export function AutomationTrigger({ scriptId, triggerId, label, available = true }: { scriptId: string; triggerId: string; label: string; available?: boolean }) {
  const pending = useRef(false)
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState('')
  const run = async () => {
    if (pending.current) return
    pending.current = true; setBusy(true); setError('')
    try { await automationAction('trigger', { script_id: scriptId, trigger_id: triggerId }) }
    catch (reason) { setError(errorMessage(reason)) }
    finally { pending.current = false; setBusy(false) }
  }
  return <div><button className="button primary" disabled={!available || busy} onClick={() => void run()}>{busy ? 'Scheduling…' : label}</button>{!available && <p className="muted small">Unavailable</p>}{error && <p className="error" role="alert">{error}</p>}</div>
}

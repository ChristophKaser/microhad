import { form, jsonRequest, post } from './api'
export interface AutomationScript {
  filename: string; id: string; enabled: boolean; loaded: boolean; generation: number
  loaded_at: number; validated_at: number; last_event_at: number; last_callback_at: number
  listeners: number; timers: number; coroutines: number; error: string
}
export interface AutomationLog {
  record_id: number; script_id: string; generation: number; at: number
  level: 'debug' | 'info' | 'warn' | 'error'; operation: string; message: string
  fields: Record<string, unknown>; line?: number
}
export const listAutomations = () => jsonRequest<{ scripts: AutomationScript[] }>('/api/automations')
export const readAutomation = (name: string) => jsonRequest<{ source: string }>(`/api/automations/${encodeURIComponent(name)}`)
export const readAutomationLogs = () => jsonRequest<{ records: AutomationLog[] }>('/api/automations/logs')
export const automationAction = (action: string, fields: Record<string, string>) => post(`/api/automations/${action}`, form(fields))
export function visibleAutomationLogs(records: AutomationLog[], id: string, severity: string, since: number, cleared: number) {
  return records.filter(row => row.script_id === id && (!severity || row.level === severity) && row.at >= since && row.record_id > cleared)
}
export function disabledFilename(name: string, enabled: boolean) { return enabled ? name.replace(/\.disabled$/, '') : `${name}.disabled` }
export function validAutomationFilename(name: string) { return /^[a-zA-Z0-9_-]{1,96}\.lua(?:\.disabled)?$/.test(name) }

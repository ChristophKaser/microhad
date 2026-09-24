import { useCallback, useEffect, useRef, useState } from 'react'
import type { Status } from './api'
import { errorMessage, jsonRequest } from './api'

export function useGatewayData() {
  const [status, setStatus] = useState<Status | null>(null)
  const [statusError, setStatusError] = useState('')
  const [logs, setLogs] = useState<string[]>([])
  const [logsError, setLogsError] = useState('')
  const statusRequest = useRef(0)
  const logsRequest = useRef(0)
  const statusPending = useRef(false)
  const logsPending = useRef(false)
  const active = useRef(true)

  const refreshStatus = useCallback(async (force = true) => {
    if (!force && statusPending.current) return
    const request = ++statusRequest.current
    statusPending.current = true
    try {
      const nextStatus = await jsonRequest<Status>('/api/status')
      if (active.current && request === statusRequest.current) {
        setStatus(nextStatus)
        setStatusError('')
      }
    } catch (error) {
      if (active.current && request === statusRequest.current) setStatusError(errorMessage(error))
    } finally {
      if (request === statusRequest.current) statusPending.current = false
    }
  }, [])

  const refreshLogs = useCallback(async () => {
    if (logsPending.current) return
    const request = ++logsRequest.current
    logsPending.current = true
    try {
      const response = await jsonRequest<{ lines: string[] }>('/api/logs')
      if (active.current && request === logsRequest.current) {
        setLogs(response.lines)
        setLogsError('')
      }
    } catch (error) {
      if (active.current && request === logsRequest.current) setLogsError(errorMessage(error))
    } finally {
      if (request === logsRequest.current) logsPending.current = false
    }
  }, [])

  useEffect(() => {
    active.current = true
    const refresh = () => {
      void refreshStatus(false)
      void refreshLogs()
    }
    refresh()
    const timer = window.setInterval(refresh, 2000)
    return () => {
      active.current = false
      window.clearInterval(timer)
    }
  }, [refreshStatus, refreshLogs])

  return { status, statusError, logs, logsError, refreshStatus }
}

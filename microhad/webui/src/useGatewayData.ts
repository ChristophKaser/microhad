import { useCallback, useEffect, useRef, useState } from 'react'
import type { Status } from './api'
import { errorMessage, jsonRequest } from './api'

function timestampOf(line: string): number | null {
  const match = line.match(/^(\d+)\s/)
  if (!match) return null
  const value = Number(match[1])
  return Number.isSafeInteger(value) ? value : null
}

export function useGatewayData(diagnosticsActive: boolean) {
  const [status, setStatus] = useState<Status | null>(null)
  const [statusError, setStatusError] = useState('')
  const [logs, setLogs] = useState<string[]>([])
  const [logsError, setLogsError] = useState('')
  const [logsHasMore, setLogsHasMore] = useState(false)
  const statusRequest = useRef(0)
  const logsRequest = useRef(0)
  const statusPending = useRef(false)
  const logsPending = useRef(false)
  const logsLoaded = useRef(false)
  const logsActivated = useRef(false)
  const latestLogTimestamp = useRef<number | null>(null)
  const olderCursor = useRef<number | null>(null)
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
      const since = latestLogTimestamp.current
      const response = await jsonRequest<{ lines: string[]; next_before?: number; has_more?: boolean }>(
        logsLoaded.current ? `/api/logs?since=${since ?? 0}` : '/api/logs?limit=200',
      )
      if (active.current && request === logsRequest.current) {
        if (!logsLoaded.current) {
          setLogs(response.lines)
          logsLoaded.current = true
          olderCursor.current = response.next_before ?? null
          setLogsHasMore(response.has_more ?? false)
        } else {
          const boundary = since ?? 0
          setLogs(current => {
            const remaining = new Map<string, number>()
            for (const line of current) {
              const timestamp = timestampOf(line)
              if (timestamp !== null && timestamp >= boundary) {
                const key = `${timestamp}\u0000${line}`
                remaining.set(key, (remaining.get(key) ?? 0) + 1)
              }
            }
            const additions: string[] = []
            for (const line of response.lines) {
              const timestamp = timestampOf(line)
              const key = timestamp === null ? '' : `${timestamp}\u0000${line}`
              const count = key ? remaining.get(key) ?? 0 : 0
              if (count > 0) remaining.set(key, count - 1)
              else additions.push(line)
            }
            return additions.length ? [...current, ...additions] : current
          })
        }
        for (const line of response.lines) {
          const timestamp = timestampOf(line)
          if (timestamp !== null && (latestLogTimestamp.current === null || timestamp > latestLogTimestamp.current))
            latestLogTimestamp.current = timestamp
        }
        setLogsError('')
      }
    } catch (error) {
      if (active.current && request === logsRequest.current) setLogsError(errorMessage(error))
    } finally {
      if (request === logsRequest.current) logsPending.current = false
    }
  }, [])

  const loadOlderLogs = useCallback(async () => {
    if (!logsHasMore || olderCursor.current === null || logsPending.current) return false
    logsPending.current = true
    try {
      const response = await jsonRequest<{ lines: string[]; next_before: number; has_more: boolean }>(
        `/api/logs?limit=500&before=${olderCursor.current}`,
      )
      if (active.current) {
        setLogs(current => [...response.lines, ...current])
        olderCursor.current = response.next_before || null
        setLogsHasMore(response.has_more)
        setLogsError('')
        return true
      }
    } catch (error) {
      if (active.current) setLogsError(errorMessage(error))
    } finally {
      logsPending.current = false
    }
    return false
  }, [logsHasMore])

  useEffect(() => {
    active.current = true
    const refresh = () => {
      void refreshStatus(false)
      if (diagnosticsActive) logsActivated.current = true
      if (logsActivated.current) void refreshLogs()
    }
    refresh()
    const timer = window.setInterval(refresh, 2000)
    return () => {
      active.current = false
      window.clearInterval(timer)
    }
  }, [diagnosticsActive, refreshStatus, refreshLogs])

  return { status, statusError, logs, logsError, logsHasMore, loadOlderLogs, refreshStatus }
}

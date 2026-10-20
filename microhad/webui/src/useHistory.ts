import { useEffect, useState } from 'react'
import type { HistorySample } from './api'
import { errorMessage } from './api'
import { historyRangeSeconds, loadHistoryPages, mergeHistorySamples } from './historyLoader'
import type { HistoryRange } from './historyLoader'

type HistoryState = { key: string; samples: HistorySample[]; now: number; loading: boolean; error: string }
const emptyHistory = (key: string, loading: boolean): HistoryState => ({ key, samples: [], now: Math.floor(Date.now() / 1000), loading, error: '' })

export function useHistory(id: string, range: HistoryRange, refresh = false): HistoryState {
  const key = `${id}:${range}`
  const [state, setState] = useState(() => emptyHistory(key, Boolean(id)))
  useEffect(() => {
    const controller = new AbortController()
    let timer = 0
    let complete = false
    setState(emptyHistory(key, Boolean(id)))
    if (!id) return () => controller.abort()
    const load = async () => {
      setState(previous => ({ ...previous, loading: true, error: '' }))
      const now = Math.floor(Date.now() / 1000)
      const since = range === 'all' ? undefined : Math.max(0, now - historyRangeSeconds[range])
      try {
        await loadHistoryPages(id, since, controller.signal, samples => {
          setState(previous => ({ key, now, loading: true, error: '', samples: complete ? mergeHistorySamples(previous.samples, samples) : samples }))
        }, complete ? 2000 : 10000)
        complete = true
      } catch (cause) {
        if (!controller.signal.aborted) setState(previous => ({ ...previous, error: errorMessage(cause) }))
      } finally {
        if (!controller.signal.aborted) {
          setState(previous => ({ ...previous, loading: false }))
          if (refresh) timer = window.setTimeout(() => { void load() }, 30000)
        }
      }
    }
    void load()
    return () => { controller.abort(); window.clearTimeout(timer) }
  }, [id, range, refresh, key])
  return state.key === key ? state : emptyHistory(key, Boolean(id))
}

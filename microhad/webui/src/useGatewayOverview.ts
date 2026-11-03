import { startVisiblePolling } from './visiblePolling'
import { useEffect, useState } from 'react'
import type { ControllerStats } from './api'
import { jsonRequest } from './api'
import type { ThreadStatus } from './matter'

export function useGatewayOverview() {
  const [thread, setThread] = useState<ThreadStatus | null>(null)
  const [resources, setResources] = useState<ControllerStats | null>(null)
  useEffect(() => {
    let active = true
    let pending = false
    const refresh = async () => {
      if (pending) return
      pending = true
      const [threadResult, statsResult] = await Promise.allSettled([
        jsonRequest<ThreadStatus>('/api/thread/status'),
        jsonRequest<ControllerStats>('/api/controller-stats'),
      ])
      if (active) {
        if (threadResult.status === 'fulfilled') setThread(threadResult.value)
        if (statsResult.status === 'fulfilled') setResources(statsResult.value)
      }
      pending = false
    }
    const stop = startVisiblePolling(refresh, 10000)
    return () => { active = false; stop() }
  }, [])
  return { thread, resources }
}

import type { HistoryPage, HistorySample } from './api'
import { jsonRequest } from './api'
import { historyUnit } from './history'

export type HistoryRange = 'day' | 'week' | 'all'
export const historyRangeSeconds = { day: 86400, week: 604800, all: Infinity }

export function mergeHistorySamples(previous: HistorySample[], incoming: HistorySample[]): HistorySample[] {
  const byCursor = new Map(previous.map(sample => [sample[0], sample]))
  for (const sample of incoming) byCursor.set(sample[0], sample)
  return [...byCursor.values()].sort((left, right) => left[0] - right[0]).slice(-10000)
}

export async function loadHistoryPages(
  id: string, since: number | undefined, signal: AbortSignal,
  onPage: (samples: HistorySample[]) => void, limit = 10000,
): Promise<HistorySample[]> {
  let before = 0
  let loaded: HistorySample[] = []
  do {
    if (signal.aborted) throw new DOMException('History request cancelled', 'AbortError')
    const query = new URLSearchParams()
    if (since !== undefined) query.set('since', String(since))
    if (before) query.set('before', String(before))
    const search = query.toString()
    const page = await jsonRequest<HistoryPage>(`/api/devices/${encodeURIComponent(id)}/history${search ? `?${search}` : ''}`, { signal })
    if (signal.aborted) throw new DOMException('History request cancelled', 'AbortError')
    loaded = mergeHistorySamples(loaded, page.samples).slice(-limit)
    onPage(loaded)
    if (before && page.next_before >= before) throw new Error('History pagination did not advance')
    before = page.next_before
  } while (before && loaded.length < limit)
  return loaded
}

// Include the last known value at the left edge, independently for each unit.
export function historyWindow(samples: HistorySample[], start: number, now: number): HistorySample[] {
  const visible = samples.filter(sample => sample[1] >= start && sample[1] <= now)
  const previous = new Map<string, HistorySample>()
  for (const sample of samples) {
    if (sample[1] >= start) continue
    const key = `${sample[2]}:${historyUnit(sample[2], sample)}`
    const prior = previous.get(key)
    if (!prior || sample[1] > prior[1] || (sample[1] === prior[1] && sample[0] > prior[0])) previous.set(key, sample)
  }
  for (const sample of previous.values()) visible.push([sample[0], start, sample[2], sample[3], sample[4]])
  return visible.sort((left, right) => left[1] - right[1] || left[0] - right[0])
}

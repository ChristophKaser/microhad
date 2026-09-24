import { useEffect, useMemo, useState } from 'react'
import type { Device, HistoryPage, HistorySample } from './api'
import { errorMessage, jsonRequest } from './api'

type Range = 'day' | 'week' | 'all'
type Metric = 1 | 2 | 3

const rangeSeconds: Record<Range, number> = { day: 86400, week: 604800, all: Infinity }
const metricName: Record<Metric, string> = { 1: 'Temperature', 2: 'Humidity', 3: 'Contact' }
const metricUnit: Record<Metric, string> = { 1: '°C', 2: '%', 3: '' }

function Chart({ samples, metric, now, start }: { samples: HistorySample[]; metric: Metric; now: number; start: number }) {
  const values = samples.filter((sample) => sample[2] === metric)
  if (!values.length) return <div className="history-empty">No {metricName[metric].toLowerCase()} samples in this range.</div>
  const first = Math.min(start, values[0][1])
  const duration = Math.max(1, now - first)
  const x = (sample: HistorySample) => 48 + ((sample[1] - first) / duration) * 650
  const numeric = values.map((sample) => sample[3] / 100)
  const low = metric === 3 ? 0 : Math.floor(Math.min(...numeric) - 1)
  const high = metric === 3 ? 1 : Math.ceil(Math.max(...numeric) + 1)
  const y = (sample: HistorySample) => metric === 3
    ? (sample[3] ? 38 : 168)
    : 168 - ((sample[3] / 100 - low) / Math.max(1, high - low)) * 130
  let path = `M ${x(values[0]).toFixed(1)} ${y(values[0]).toFixed(1)}`
  for (let i = 1; i < values.length; i++) {
    const point = values[i]
    if (metric === 3) path += ` H ${x(point).toFixed(1)} V ${y(point).toFixed(1)}`
    else path += ` L ${x(point).toFixed(1)} ${y(point).toFixed(1)}`
  }
  if (metric === 3) path += ' H 698'
  const latest = values[values.length - 1]
  const latestLabel = metric === 3 ? (latest[3] ? 'Open' : 'Closed') : `${(latest[3] / 100).toFixed(2)} ${metricUnit[metric]}`
  return (
    <section className="history-chart">
      <div className="history-chart-title"><h3>{metricName[metric]}</h3><span>{latestLabel} · {values.length} samples</span></div>
      <svg viewBox="0 0 720 215" role="img" aria-label={`${metricName[metric]} history, latest ${latestLabel}`}>
        <line x1="48" y1="38" x2="698" y2="38" className="history-grid" />
        <line x1="48" y1="168" x2="698" y2="168" className="history-grid" />
        <text x="4" y="42">{metric === 3 ? 'Open' : `${high}${metricUnit[metric]}`}</text>
        <text x="4" y="172">{metric === 3 ? 'Closed' : `${low}${metricUnit[metric]}`}</text>
        <path d={path} className={`history-line metric-${metric}`} />
        <circle cx={x(latest)} cy={y(latest)} r="4" className={`history-point metric-${metric}`} />
        <text x="48" y="202">{new Date(first * 1000).toLocaleString()}</text>
        <text x="698" y="202" textAnchor="end">{new Date(now * 1000).toLocaleString()}</text>
      </svg>
    </section>
  )
}

export function HistoryDialog({ device, onClose }: { device: Device; onClose: () => void }) {
  const [samples, setSamples] = useState<HistorySample[]>([])
  const [range, setRange] = useState<Range>('day')
  const [loading, setLoading] = useState(true)
  const [error, setError] = useState('')
  const [now, setNow] = useState(() => Math.floor(Date.now() / 1000))
  useEffect(() => {
    let active = true
    const load = async () => {
      setLoading(true)
      setError('')
      try {
        let before = 0
        const loaded: HistorySample[] = []
        do {
          const page = await jsonRequest<HistoryPage>(`/api/devices/${encodeURIComponent(device.ieee_address)}/history${before ? `?before=${before}` : ''}`)
          loaded.push(...page.samples)
          before = page.next_before
        } while (before && loaded.length < 10000)
        if (active) {
          setSamples(loaded.sort((a, b) => a[0] - b[0]))
          setNow(Math.floor(Date.now() / 1000))
        }
      } catch (cause) {
        if (active) setError(errorMessage(cause))
      } finally {
        if (active) setLoading(false)
      }
    }
    void load()
    return () => { active = false }
  }, [device.ieee_address])
  const start = range === 'all' ? (samples[0]?.[1] ?? now) : now - rangeSeconds[range]
  const visible = useMemo(() => samples.filter((sample) => sample[1] >= start && sample[1] <= now), [samples, start, now])
  return (
    <div className="modal" role="presentation" onMouseDown={(event) => { if (event.target === event.currentTarget) onClose() }}>
      <div className="dialog wide history-dialog" role="dialog" aria-modal="true" aria-label={`History for ${device.name}`}>
        <div className="dialog-heading"><div><h2>{device.name} history</h2><p className="muted small">Stored in gateway RAM until overwritten or rebooted</p></div><button className="icon-button" onClick={onClose} aria-label="Close">✕</button></div>
        <div className="history-ranges" role="group" aria-label="History range">
          {([['day', '24 hours'], ['week', '7 days'], ['all', 'All retained']] as const).map(([key, label]) =>
            <button key={key} className={`button ${range === key ? 'primary' : ''}`} onClick={() => setRange(key)}>{label}</button>)}
        </div>
        {loading && <p className="muted">Loading history…</p>}
        {error && <p className="error" role="alert">{error}</p>}
        {!loading && !error && <>
          {device.has_temperature && <Chart samples={visible} metric={1} now={now} start={start} />}
          {device.has_humidity && <Chart samples={visible} metric={2} now={now} start={start} />}
          {device.has_contact && <Chart samples={visible} metric={3} now={now} start={start} />}
        </>}
      </div>
    </div>
  )
}

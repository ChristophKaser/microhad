import { useEffect, useMemo, useRef, useState } from 'react'
import { Chart as ChartJS, LineController, LineElement, LinearScale, PointElement, Tooltip } from 'chart.js'
import type { Device, HistoryPage, HistorySample } from './api'
import { errorMessage, jsonRequest } from './api'

type Range = 'day' | 'week' | 'all'
type Metric = 1 | 2 | 3

const rangeSeconds: Record<Range, number> = { day: 86400, week: 604800, all: Infinity }
const metricName: Record<Metric, string> = { 1: 'Temperature', 2: 'Humidity', 3: 'Contact' }
const metricUnit: Record<Metric, string> = { 1: '°C', 2: '%', 3: '' }
const metricColor: Record<Metric, string> = { 1: '#d97543', 2: '#3384aa', 3: '#7554a4' }

ChartJS.register(LineController, LineElement, LinearScale, PointElement, Tooltip)

function Chart({ samples, metric, now, start }: { samples: HistorySample[]; metric: Metric; now: number; start: number }) {
  const values = samples.filter((sample) => sample[2] === metric)
  const canvas = useRef<HTMLCanvasElement>(null)
  const latest = values[values.length - 1]
  const latestLabel = latest && (metric === 3 ? (latest[3] ? 'Open' : 'Closed') : `${(latest[3] / 100).toFixed(2)} ${metricUnit[metric]}`)

  useEffect(() => {
    if (!canvas.current || !values.length) return
    const first = Math.min(start, values[0][1])
    const numeric = values.map((sample) => metric === 3 ? sample[3] : sample[3] / 100)
    const low = metric === 3 ? 0 : Math.floor(Math.min(...numeric) - 1)
    const high = metric === 3 ? 1 : Math.ceil(Math.max(...numeric) + 1)
    const points = values.map((sample) => ({ x: sample[1], y: metric === 3 ? sample[3] : sample[3] / 100 }))
    if (metric === 3 && points[points.length - 1].x < now) points.push({ x: now, y: points[points.length - 1].y })
    const chart = new ChartJS(canvas.current, {
      type: 'line',
      data: { datasets: [{
        label: metricName[metric], data: points, borderColor: metricColor[metric],
        backgroundColor: metricColor[metric], borderWidth: 2.5, tension: 0,
        stepped: metric === 3 ? 'after' : false,
        pointRadius: (context) => context.dataIndex === values.length - 1 ? 4 : 0,
        pointHoverRadius: 5,
      }] },
      options: {
        responsive: true, maintainAspectRatio: false, animation: false,
        parsing: false, normalized: false,
        interaction: { intersect: false, mode: 'nearest' },
        plugins: {
          legend: { display: false },
          tooltip: {
            filter: (context) => context.dataIndex < values.length,
            callbacks: {
              title: (items) => items.length ? new Date(Number(items[0].parsed.x) * 1000).toLocaleString() : '',
              label: (item) => metric === 3 ? (item.parsed.y ? 'Open' : 'Closed') : `${Number(item.parsed.y).toFixed(2)} ${metricUnit[metric]}`,
            },
          },
        },
        scales: {
          x: {
            type: 'linear', min: Math.min(first, now - 1), max: now,
            grid: { color: '#e9eef3' },
            ticks: { maxTicksLimit: 4, maxRotation: 0, callback: (value) => {
              const date = new Date(Number(value) * 1000)
              return now - first <= 86400 ? date.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' }) : date.toLocaleDateString([], { month: 'short', day: 'numeric' })
            } },
          },
          y: {
            min: low, max: high, grid: { color: '#dbe6ed' },
            ticks: { maxTicksLimit: metric === 3 ? 2 : 5, stepSize: metric === 3 ? 1 : undefined,
              callback: (value) => metric === 3 ? (Number(value) ? 'Open' : 'Closed') : `${value}${metricUnit[metric]}` },
          },
        },
      },
    })
    return () => chart.destroy()
  }, [values, metric, now, start])

  if (!latest) return <div className="history-empty">No {metricName[metric].toLowerCase()} samples in this range.</div>
  return (
    <section className="history-chart">
      <div className="history-chart-title"><h3>{metricName[metric]}</h3><span>{latestLabel} · {values.length} samples</span></div>
      <div className="history-canvas"><canvas ref={canvas} role="img" aria-label={`${metricName[metric]} history, latest ${latestLabel}`} /></div>
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

import { useEffect, useMemo, useRef, useState } from 'react'
import { Chart as ChartJS, LineController, LineElement, LinearScale, PointElement, Tooltip } from 'chart.js'
import type { Device, HistoryMetric, HistoryPage, HistorySample } from './api'
import { errorMessage, jsonRequest } from './api'
import { historyDeviceId, historyDiscrete, historyLabel, historyName, historyTick, historyUnit, historyY } from './history'

type Range = 'day' | 'week' | 'all'
const rangeSeconds: Record<Range, number> = { day: 86400, week: 604800, all: Infinity }
const metricColor: Record<HistoryMetric, string> = {
  1: '#d97543', 2: '#3384aa', 3: '#7554a4', 4: '#b77928', 5: '#4b956a', 6: '#71859a',
  7: '#9a5b99', 8: '#6d8293', 9: '#368578', 10: '#d2a635', 11: '#427fb0',
}

ChartJS.register(LineController, LineElement, LinearScale, PointElement, Tooltip)

function Chart({ samples, metric, unit, now, start }: { samples: HistorySample[]; metric: HistoryMetric; unit: string; now: number; start: number }) {
  const values = samples.filter((sample) => sample[2] === metric && historyUnit(metric, sample) === unit)
  const canvas = useRef<HTMLCanvasElement>(null)
  const latest = values[values.length - 1]
  const latestLabel = latest && historyLabel(metric, latest)

  useEffect(() => {
    if (!canvas.current || !values.length) return
    const first = Math.min(start, values[0][1])
    const numeric = values.map((sample) => historyY(metric, sample))
    const discrete = historyDiscrete(metric)
    const low = discrete ? 0 : Math.floor(Math.min(...numeric) - 1)
    const high = discrete ? (metric === 9 ? 5 : 1) : Math.ceil(Math.max(...numeric) + 1)
    const points = values.map((sample) => ({ x: sample[1], y: historyY(metric, sample) }))
    if (points[points.length - 1].x < now) points.push({ x: now, y: points[points.length - 1].y })
    const chart = new ChartJS(canvas.current, {
      type: 'line',
      data: { datasets: [{
        label: historyName(metric), data: points, borderColor: metricColor[metric],
        backgroundColor: metricColor[metric], borderWidth: 2.5, tension: 0,
        stepped: discrete ? 'after' : false,
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
                label: (item) => historyLabel(metric, values[item.dataIndex]),
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
            ticks: { maxTicksLimit: discrete ? (metric === 9 ? 6 : 2) : 5, stepSize: discrete ? 1 : undefined,
              callback: (value) => {
                const tick = historyTick(metric, Number(value))
                return unit ? `${tick} ${unit}` : tick
              } },
          },
        },
      },
    })
    return () => chart.destroy()
  }, [values, metric, unit, now, start])

  if (!latest) return <div className="history-empty">No {historyName(metric).toLowerCase()} samples in this range.</div>
  return (
    <section className="history-chart">
      <div className="history-chart-title"><h3>{historyName(metric)}{unit ? ` (${unit})` : ''}</h3><span>{latestLabel} · {values.length} samples</span></div>
      <div className="history-canvas"><canvas ref={canvas} role="img" aria-label={`${historyName(metric)} history, latest ${latestLabel}`} /></div>
    </section>
  )
}

export function HistoryDialog({ device, onClose }: { device: Device; onClose: () => void }) {
  const [samples, setSamples] = useState<HistorySample[]>([])
  const [range, setRange] = useState<Range>('day')
  const [loading, setLoading] = useState(true)
  const [error, setError] = useState('')
  const [now, setNow] = useState(() => Math.floor(Date.now() / 1000))
  const metrics: HistoryMetric[] = [
    ...(device.has_temperature ? [1 as const] : []),
    ...(device.has_humidity ? [2 as const] : []),
    ...(device.has_contact ? [3 as const] : []),
    ...(device.has_power ? [4 as const] : []),
    ...(device.has_occupancy ? [5 as const] : []),
    ...(device.has_battery ? [6 as const] : []),
    ...(device.has_co2 ? [7 as const] : []),
    ...(device.has_pm25 ? [8 as const] : []),
    ...(device.has_air_quality ? [9 as const] : []),
    ...(device.has_light || device.has_actor ? [10 as const] : []),
    ...(device.has_brightness ? [11 as const] : []),
  ]
  const historyId = historyDeviceId(device)
  const series = metrics.flatMap((metric) => {
    const units = [...new Set(samples.filter((sample) => sample[2] === metric).map((sample) => historyUnit(metric, sample)))]
    return (units.length ? units : [historyUnit(metric)]).map((unit) => ({ metric, unit }))
  })
  useEffect(() => {
    let active = true
    const load = async () => {
      setLoading(true)
      setError('')
      try {
        let before = 0
        const loaded: HistorySample[] = []
        do {
          const page = await jsonRequest<HistoryPage>(`/api/devices/${encodeURIComponent(historyId)}/history${before ? `?before=${before}` : ''}`)
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
  }, [historyId])
  const start = range === 'all' ? (samples[0]?.[1] ?? now) : now - rangeSeconds[range]
  const visible = useMemo(() => {
    const current = samples.filter((sample) => sample[1] >= start && sample[1] <= now)
    for (const metric of metrics) {
      const prior = [...samples].reverse().find((sample) => sample[2] === metric && sample[1] < start)
      if (prior) current.unshift([prior[0], start, metric, prior[3], prior[4]])
    }
    return current.sort((left, right) => left[1] - right[1])
  }, [samples, start, now, metrics])
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
          {series.map(({ metric, unit }) => <Chart key={`${metric}-${unit}`} samples={visible} metric={metric} unit={unit} now={now} start={start} />)}
        </>}
      </div>
    </div>
  )
}

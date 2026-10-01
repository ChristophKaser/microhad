import { useEffect, useRef, useState } from 'react'
import { Chart as ChartJS, ArcElement, DoughnutController, Tooltip } from 'chart.js'
import { errorMessage, jsonRequest } from './api'
import type { ControllerStats as ControllerStatsData } from './api'

ChartJS.register(DoughnutController, ArcElement, Tooltip)

function formatBytes(value: number | null) {
  if (value === null) return 'Unavailable'
  if (value < 1024) return `${value} B`
  const units = ['KB', 'MB', 'GB', 'TB']
  let amount = value / 1024
  let unit = units[0]
  for (let i = 1; amount >= 1024 && i < units.length; i++) { amount /= 1024; unit = units[i] }
  return `${amount.toFixed(amount >= 10 ? 0 : 1)} ${unit}`
}

function formatDuration(seconds: number | null) {
  if (seconds === null) return 'Unavailable'
  const days = Math.floor(seconds / 86400)
  const hours = Math.floor((seconds % 86400) / 3600)
  const minutes = Math.floor((seconds % 3600) / 60)
  const remainder = seconds % 60
  return [days ? `${days}d` : '', hours || days ? `${hours}h` : '', minutes || hours || days ? `${minutes}m` : `${remainder}s`].filter(Boolean).join(' ')
}

function Donut({ title, free, total }: { title: string; free: number | null; total: number | null }) {
  const canvas = useRef<HTMLCanvasElement>(null)
  const chartRef = useRef<ChartJS<'doughnut', number[], string> | null>(null)
  useEffect(() => {
    if (!canvas.current || free === null || total === null || total <= 0) return
    const safeFree = Math.min(total, Math.max(0, free))
    if (chartRef.current) {
      chartRef.current.data.datasets[0].data = [safeFree, Math.max(0, total - safeFree)]
      chartRef.current.update('none')
    } else {
      chartRef.current = new ChartJS(canvas.current, {
        type: 'doughnut',
        data: { labels: ['Free', 'Used'], datasets: [{ data: [safeFree, Math.max(0, total - safeFree)], backgroundColor: ['#48a77a', '#dce5ed'], borderWidth: 0 }] },
        options: { responsive: true, maintainAspectRatio: false, cutout: '72%', animation: false, plugins: { legend: { display: false }, tooltip: { callbacks: { label: context => `${context.label}: ${formatBytes(Number(context.raw))}` } } } },
      })
    }
  }, [free, total])
  useEffect(() => () => { chartRef.current?.destroy(); chartRef.current = null }, [])
  const percent = free !== null && total ? Math.round(Math.min(100, Math.max(0, free / total * 100))) : null
  return <article className="controller-donut panel">
    <h3>{title}</h3>
    {free !== null && total !== null && total > 0
      ? <div className="controller-donut-chart"><canvas ref={canvas} role="img" aria-label={`${title}: ${formatBytes(free)} free of ${formatBytes(total)}`} /><strong>{percent}%<small>free</small></strong></div>
      : <p className="muted">Unavailable</p>}
    <p className="controller-donut-caption">{formatBytes(free)} free of {formatBytes(total)}</p>
  </article>
}

export function ControllerStats() {
  const [stats, setStats] = useState<ControllerStatsData | null>(null)
  const [samples, setSamples] = useState<{ time: number; value: number }[]>([])
  const [error, setError] = useState('')
  useEffect(() => {
    let active = true
    let pending = false
    const refresh = async () => {
      if (pending) return
      pending = true
      try {
        const next = await jsonRequest<ControllerStatsData>('/api/controller-stats')
        if (active) {
          setStats(next)
          setError('')
          if (next.cpu_usage_percent !== null) setSamples(current => [...current, { time: Date.now(), value: next.cpu_usage_percent! }].slice(-60))
        }
      } catch (cause) { if (active) setError(errorMessage(cause)) }
      finally { pending = false }
    }
    void refresh()
    const timer = window.setInterval(() => { void refresh() }, 2000)
    return () => { active = false; window.clearInterval(timer) }
  }, [])
  const points = samples.map((sample, index) => `${samples.length < 2 ? 50 : index * 100 / (samples.length - 1)},${100 - Math.min(100, Math.max(0, sample.value))}`).join(' ')
  const cpu = stats?.cpu_usage_percent ?? null
  return <section className="controller-stats">
    <div className="section-heading"><div><p className="eyebrow">Controller</p><h2>System statistics</h2></div><span className="muted small">Refreshes every 2 seconds while this tab is open</span></div>
    {error && <p className="error" role="alert">Unable to load controller stats: {error}</p>}
    <div className="controller-stats-grid">
      <article className="panel controller-cpu"><div className="controller-stat-heading"><div><h3>CPU usage</h3><strong>{cpu === null ? 'Waiting for sample' : `${cpu.toFixed(1)}%`}</strong></div><span className="muted small">Recent polling samples</span></div>
        <div className="controller-cpu-graph"><svg viewBox="0 0 100 100" preserveAspectRatio="none" role="img" aria-label="CPU usage during the current visit"><line x1="0" y1="0" x2="100" y2="0" /><line x1="0" y1="50" x2="100" y2="50" /><line x1="0" y1="100" x2="100" y2="100" />{samples.length > 0 && <polyline points={points} />}</svg></div>
      </article>
      <Donut title="Memory" free={stats?.memory_free_bytes ?? null} total={stats?.memory_total_bytes ?? null} />
      <Donut title="Runtime storage" free={stats?.storage_free_bytes ?? null} total={stats?.storage_total_bytes ?? null} />
    </div>
    <div className="controller-detail-grid panel">
      <div><span>Device uptime</span><strong>{formatDuration(stats?.device_uptime_seconds ?? null)}</strong></div>
      <div><span>Service uptime</span><strong>{formatDuration(stats?.service_uptime_seconds ?? null)}</strong></div>
      <div><span>Serial connection</span><strong>{formatDuration(stats?.serial_uptime_seconds ?? null)}</strong></div>
      <div><span>Process memory</span><strong>{formatBytes(stats?.process_memory_bytes ?? null)}</strong></div>
      <div><span>Log file</span><strong>{formatBytes(stats?.log_file_bytes ?? null)}</strong></div>
      <div><span>Values history file</span><strong>{formatBytes(stats?.values_file_bytes ?? null)}</strong></div>
      <div><span>Load average · 1 min</span><strong>{stats?.cpu_load_1?.toFixed(2) ?? 'Unavailable'}</strong></div>
      <div><span>Load average · 5 min</span><strong>{stats?.cpu_load_5?.toFixed(2) ?? 'Unavailable'}</strong></div>
      <div><span>Load average · 15 min</span><strong>{stats?.cpu_load_15?.toFixed(2) ?? 'Unavailable'}</strong></div>
    </div>
    <article className="panel controller-threads">
      <div className="controller-stat-heading"><div><h3>CPU by thread</h3><span className="muted small">Percent of one core · current poll sample</span></div><span className="muted small">{stats?.threads.length ?? 0} threads</span></div>
      <div className="controller-thread-table-wrap"><table><thead><tr><th>Thread</th><th>TID</th><th>CPU</th></tr></thead><tbody>
        {(stats?.threads ?? []).map(thread => <tr key={`${thread.tid}-${thread.name}`}><td>{thread.name}</td><td>{thread.tid}</td><td>{thread.cpu_usage_percent === null ? <span className="muted">Sampling…</span> : <div className="controller-thread-usage"><span>{thread.cpu_usage_percent.toFixed(1)}%</span><i><b style={{ width: `${Math.min(100, Math.max(0, thread.cpu_usage_percent))}%` }} /></i></div>}</td></tr>)}
        {stats && stats.threads.length === 0 && <tr><td colSpan={3} className="muted">Thread statistics unavailable.</td></tr>}
      </tbody></table></div>
    </article>
  </section>
}

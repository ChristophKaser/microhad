import { useEffect, useMemo, useRef, useState } from 'react'
import { Chart as ChartJS, LineController, LineElement, LinearScale, PointElement, Tooltip } from 'chart.js'
import type { Device, HistoryPage, HistorySample, SceneGroupSummary, Status } from './api'
import { errorMessage, jsonRequest } from './api'
import { DeviceGroups } from './devices'

export type DashboardWidget = {
  id: string
  kind: 'device' | 'group' | 'chart'
  target: string
  metric?: number
  range?: 'day' | 'week' | 'all'
}
export type DashboardConfig = { widgets: DashboardWidget[] }
export type SensorMetric = { id: number; name: string; unit: string; scale: number; available: (d: Device) => boolean }
export const sensorMetrics: SensorMetric[] = [
  { id: 1, name: 'Temperature', unit: '°C', scale: 100, available: d => d.has_temperature },
  { id: 2, name: 'Humidity', unit: '%', scale: 100, available: d => d.has_humidity },
  { id: 3, name: 'Contact', unit: '', scale: 1, available: d => d.has_contact },
  { id: 4, name: 'Power', unit: 'W', scale: 10, available: d => d.has_power },
  { id: 5, name: 'Occupancy', unit: '', scale: 1, available: d => d.has_occupancy },
  { id: 6, name: 'Battery', unit: '%', scale: 1, available: d => d.has_battery },
]
const rangeSeconds = { day: 86400, week: 604800, all: Infinity }
ChartJS.register(LineController, LineElement, LinearScale, PointElement, Tooltip)

function SensorChart({ device, metricId, range }: { device: Device | undefined; metricId: number; range: 'day' | 'week' | 'all' }) {
  const canvas = useRef<HTMLCanvasElement>(null)
  const [samples, setSamples] = useState<HistorySample[]>([])
  const [error, setError] = useState('')
  const [now, setNow] = useState(() => Math.floor(Date.now() / 1000))
  const metric = sensorMetrics.find(item => item.id === metricId)
  useEffect(() => {
    if (!device) return
    let active = true
    let timer = 0
    const load = async (initial = false) => {
      try {
        let before = 0
        const loaded: HistorySample[] = []
        do {
          const page = await jsonRequest<HistoryPage>(`/api/devices/${encodeURIComponent(device.id)}/history${before ? `?before=${before}` : ''}`)
          loaded.push(...page.samples)
          before = initial ? page.next_before : 0
        } while (initial && before && loaded.length < 10000)
        if (active) {
          setSamples(previous => {
            const bySequence = new Map<number, HistorySample>()
            for (const sample of previous) bySequence.set(sample[0], sample)
            for (const sample of loaded) bySequence.set(sample[0], sample)
            return [...bySequence.values()].sort((a,b) => a[0]-b[0]).slice(-10000)
          })
          setNow(Math.floor(Date.now() / 1000)); setError('')
        }
      } catch (cause) { if (active) setError(errorMessage(cause)) }
    }
    void load(true)
    timer = window.setInterval(() => { void load(false) }, 30000)
    return () => { active = false; window.clearInterval(timer) }
  }, [device?.id])
  const points = useMemo(() => samples.filter(sample => sample[2] === metricId).sort((a,b) => a[1]-b[1]), [samples, metricId])
  const start = range === 'all' ? (points[0]?.[1] ?? now) : now - rangeSeconds[range]
  useEffect(() => {
    if (!canvas.current || !points.length || !metric) return
    const chart = new ChartJS(canvas.current, {
      type: 'line',
      data: { datasets: [{ label: metric.name, data: points.filter(p => p[1] >= start).map(p => ({x:p[1], y: metricId === 3 || metricId === 5 ? p[3] : p[3] / metric.scale})), borderColor: '#287da5', backgroundColor: '#287da5', borderWidth: 2, stepped: metricId === 3 || metricId === 5 ? 'after' : false, pointRadius: 0 }] },
      options: { responsive: true, maintainAspectRatio: false, animation: false, parsing: false, plugins: { legend: { display: false }, tooltip: { callbacks: { label: item => metricId === 3 ? (item.parsed.y ? 'Open' : 'Closed') : metricId === 5 ? (item.parsed.y ? 'Occupied' : 'Clear') : `${item.parsed.y} ${metric.unit}` } } }, scales: { x: { type: 'linear', min: start, max: now, ticks: { maxTicksLimit: 4, callback: value => new Date(Number(value)*1000).toLocaleTimeString([], {hour:'2-digit',minute:'2-digit'}) } }, y: { ticks: { maxTicksLimit: 4, callback: value => metricId === 3 ? (Number(value) ? 'Open' : 'Closed') : metricId === 5 ? (Number(value) ? 'Occupied' : 'Clear') : `${value}${metric.unit}` } } } }
    })
    return () => chart.destroy()
  }, [points, metric, metricId, start, now])
  const latest = points[points.length-1]
  const value = !latest || !metric ? '—' : metricId === 3 ? (latest[3] ? 'Open' : 'Closed') : metricId === 5 ? (latest[3] ? 'Occupied' : 'Clear') : `${(latest[3] / metric.scale).toFixed(metric.scale === 1 ? 0 : 1)} ${metric.unit}`
  return <article className="dashboard-widget dashboard-chart"><div className="dashboard-widget-title"><div><h3>{device?.name ?? 'Unavailable device'}</h3><span>{metric?.name ?? 'Sensor'} · {value}</span></div></div>{error ? <p className="error">{error}</p> : points.length ? <div className="dashboard-chart-canvas"><canvas ref={canvas} role="img" aria-label={`${device?.name} ${metric?.name} chart`} /></div> : <p className="muted small">No retained samples for this value yet.</p>}</article>
}

function GroupWidget({ group, devices, action }: { group: SceneGroupSummary | undefined; devices: Device[]; action: (url: string, body?: string) => void }) {
  if (!group) return <article className="dashboard-widget"><h3>Unavailable group</h3><p className="muted small">This saved group is no longer configured.</p></article>
  return <article className="dashboard-widget"><div className="dashboard-widget-title"><div><h3>{group.name}</h3><span>{group.members.length} light(s)</span></div><button className="button" onClick={() => action(`/api/scenes/${group.id}/off`)}>Turn off</button></div><div className="presets">{group.scenes.map(scene => <button className="button" key={scene.id} onClick={() => action(`/api/scenes/${group.id}/${scene.id}/activate`)}>▶ {scene.name}</button>)}</div><p className="muted small">Group members</p><div className="group-light-states">{group.members.map(address => { const d=devices.find(item=>item.ieee_address===address); return <span className="dashboard-member" key={address}>{d?.name ?? address} · {d ? (d.light_on ? 'On' : 'Off') : 'Unknown'}</span> })}</div></article>
}

export function Dashboard({ config, editing, devices, groups, status, action, onConfig, onSettings, onDetails, onHistory, onConnection, onEdit, onCancel, onSave, saving, loaded, configError }: {
  config: DashboardConfig; editing: boolean; devices: Device[]; groups: SceneGroupSummary[]; status: Status | null
  action: (url: string, body?: string) => void; onConfig: (value: DashboardConfig) => void
  onSettings: (device: Device) => void; onDetails: (device: Device) => void; onHistory: (device: Device) => void; onConnection: (device: Device) => void
  onEdit: () => void; onCancel: () => void; onSave: () => void; saving: boolean; loaded: boolean; configError: string
}) {
  const [kind, setKind] = useState<'device'|'group'|'chart'>('device')
  const [target, setTarget] = useState('')
  const [metric, setMetric] = useState(1)
  const pickedDevice = devices.find(device => device.id === target)
  const add = () => {
    const widget: DashboardWidget = { id: `${Date.now()}-${Math.random().toString(36).slice(2)}`, kind, target, ...(kind === 'chart' ? { metric, range: 'day' as const } : {}) }
    onConfig({ widgets: [...config.widgets, widget] })
    setTarget('')
  }
  return <section className="dashboard-view">
    <section className="overview dashboard-overview"><div className={`overview-item ${status?.online ? 'good' : 'bad'}`}><span className="overview-icon">●</span><div><span className="overview-label">Coordinator</span><strong>{status ? status.online ? 'Online' : 'Offline' : 'Checking'}</strong></div></div><div className={`overview-item ${status?.network_formed ? 'good' : status?.network_forming ? 'warn' : ''}`}><span className="overview-icon">⌘</span><div><span className="overview-label">Zigbee network</span><strong>{status?.network_formed ? 'Formed' : status?.network_forming ? 'Forming' : 'Not formed'}</strong></div></div><div className="overview-item"><span className="overview-icon">▦</span><div><span className="overview-label">Devices</span><strong>{devices.length}</strong></div></div></section>
    <div className="dashboard-heading"><div><p className="eyebrow">Your home</p><h2>Dashboard</h2></div>{editing ? <div className="toolbar"><button className="button" disabled={saving} onClick={onCancel}>Cancel</button><button className="button primary" disabled={saving} onClick={onSave}>{saving ? 'Saving…' : 'Save layout'}</button></div> : <button className="button" disabled={!loaded} onClick={onEdit}>{loaded ? 'Edit dashboard' : 'Loading…'}</button>}</div>
    {configError && <p className="error" role="alert">{configError}</p>}
    {editing && <div className="panel dashboard-editor"><strong>Add a widget</strong><div className="toolbar"><select value={kind} onChange={e => { const next=e.target.value as typeof kind; setKind(next); setTarget('') }}><option value="device">Device card</option><option value="group">Group card</option><option value="chart">Sensor chart</option></select><select value={target} onChange={e => { const value=e.target.value; setTarget(value); if(kind==='chart'){ const selectedDevice=devices.find(d=>d.id===value); const first=sensorMetrics.find(m=>selectedDevice && m.available(selectedDevice)); if(first)setMetric(first.id) } }}><option value="">Choose {kind === 'group' ? 'group' : 'device'}</option>{(kind === 'group' ? groups.map(g=>({id:String(g.id),name:g.name})) : devices.map(d=>({id:d.id,name:d.name}))).map(item=><option key={item.id} value={item.id}>{item.name}</option>)}</select>{kind === 'chart' && <select value={metric} onChange={e=>setMetric(Number(e.target.value))}>{sensorMetrics.filter(m=>pickedDevice && m.available(pickedDevice)).map(m=><option key={m.id} value={m.id}>{m.name}</option>)}</select>}<button className="button primary" disabled={!target || (kind==='chart' && !sensorMetrics.some(m=>m.id===metric && pickedDevice && m.available(pickedDevice)))} onClick={add}>Add widget</button></div><p className="muted small">Use the arrows on widgets to change their order, then save the layout.</p></div>}
    <div className="dashboard-grid">{config.widgets.map((widget,index)=>{
      const move=(direction:number)=>{ const next=[...config.widgets]; const swap=index+direction; if(swap<0||swap>=next.length)return; [next[index],next[swap]]=[next[swap],next[index]]; onConfig({widgets:next}) }
      const device=devices.find(d=>d.id===widget.target)
      const controls=editing?<div className="dashboard-widget-actions"><button className="icon-button" aria-label="Move widget up" onClick={()=>move(-1)}>↑</button><button className="icon-button" aria-label="Move widget down" onClick={()=>move(1)}>↓</button><button className="icon-button" aria-label="Remove widget" onClick={()=>onConfig({widgets:config.widgets.filter(item=>item.id!==widget.id)})}>×</button></div>:null
      if(widget.kind==='chart')return <div className="dashboard-widget-shell" key={widget.id}>{controls}<SensorChart device={device} metricId={widget.metric ?? 1} range={widget.range ?? 'day'}/>{editing&&<div className="dashboard-chart-settings"><label>Range <select value={widget.range??'day'} onChange={e=>onConfig({widgets:config.widgets.map(item=>item.id===widget.id?{...item,range:e.target.value as DashboardWidget['range']}:item)})}><option value="day">24 hours</option><option value="week">7 days</option><option value="all">All retained</option></select></label><label>Value <select value={widget.metric??1} onChange={e=>onConfig({widgets:config.widgets.map(item=>item.id===widget.id?{...item,metric:Number(e.target.value)}:item)})}>{sensorMetrics.filter(m=>device&&m.available(device)).map(m=><option key={m.id} value={m.id}>{m.name}</option>)}</select></label></div>}</div>
      if(widget.kind==='group')return <div className="dashboard-widget-shell" key={widget.id}>{controls}<GroupWidget group={groups.find(g=>String(g.id)===widget.target)} devices={devices} action={action}/></div>
      if(!device)return <div className="dashboard-widget-shell" key={widget.id}>{controls}<article className="dashboard-widget"><h3>Unavailable device</h3><p className="muted small">This saved device is no longer connected.</p></article></div>
      if(!status)return <div className="dashboard-widget-shell" key={widget.id}>{controls}<article className="dashboard-widget">Loading device…</article></div>
      const single={...status,devices:[device]}
      return <div className="dashboard-widget-shell" key={widget.id}>{controls}<DeviceGroups status={single} action={action} hideContext onSettings={()=>onSettings(device)} onDetails={()=>onDetails(device)} onHistory={()=>onHistory(device)} onConnection={()=>onConnection(device)}/></div>
    })}</div>
  </section>
}

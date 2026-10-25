import { useEffect, useMemo, useRef, useState } from 'react'
import type { CSSProperties } from 'react'
import { Chart as ChartJS, LineController, LineElement, LinearScale, PointElement, Tooltip } from 'chart.js'
import type { Device, HistoryMetric, SceneGroupSummary, Status } from './api'
import type { ControllerStats } from './api'
import type { ThreadStatus } from './matter'
import { DeviceGroups } from './devices'
import { historyRangeSeconds, historyWindow } from './historyLoader'
import { listAutomations, type AutomationScript } from './automation'
import { AutomationTrigger } from './AutomationTrigger'
import { useHistory } from './useHistory'
import { historyDeviceId, historyDiscrete, historyLabel, historyTick, historyUnit, historyY } from './history'

export type DashboardWidget = {
  id: string
  kind: 'automation_trigger' | 'gateway' | 'device' | 'group' | 'chart' | 'open_contacts' | 'present_sensors'
  target: string
  script_id?: string
  trigger_id?: string
  label?: string
  metric?: number
  range?: 'day' | 'week' | 'all'
}
export type DashboardConfig = { widgets: DashboardWidget[]; gateway_widget_initialized?: boolean }
export type SensorMetric = { id: number; name: string; unit: string; scale: number; color: string; available: (d: Device) => boolean }
export const sensorMetrics: SensorMetric[] = [
  { id: 1, name: 'Temperature', unit: '°C', scale: 100, color: '#d97543', available: d => d.has_temperature },
  { id: 2, name: 'Humidity', unit: '%', scale: 100, color: '#3384aa', available: d => d.has_humidity },
  { id: 3, name: 'Contact', unit: '', scale: 1, color: '#7554a4', available: d => d.has_contact && !d.has_water_leak },
  { id: 4, name: 'Power', unit: 'W', scale: 10, color: '#b77928', available: d => d.has_power },
  { id: 5, name: 'Occupancy', unit: '', scale: 1, color: '#4b956a', available: d => d.has_occupancy },
  { id: 6, name: 'Battery', unit: '%', scale: 1, color: '#71859a', available: d => d.has_battery },
  { id: 7, name: 'CO₂', unit: 'ppm', scale: 1, color: '#9a5b99', available: d => d.has_co2 },
  { id: 8, name: 'PM2.5', unit: 'ug/m3', scale: 1, color: '#6d8293', available: d => d.has_pm25 },
  { id: 9, name: 'Air quality', unit: '', scale: 1, color: '#368578', available: d => d.has_air_quality },
  { id: 10, name: 'Light', unit: '', scale: 1, color: '#d2a635', available: d => d.has_light || d.has_actor },
  { id: 11, name: 'Brightness', unit: '%', scale: 1, color: '#427fb0', available: d => d.has_brightness },
  { id: 12, name: 'CO₂ mass concentration', unit: 'ug/m3', scale: 1, color: '#ad74a8', available: d => d.has_co2 },
  { id: 13, name: 'CO₂ particle concentration', unit: 'particles/m3', scale: 1, color: '#865c91', available: d => d.has_co2 },
  { id: 14, name: 'CO₂ activity concentration', unit: 'Bq/m3', scale: 1, color: '#774c81', available: d => d.has_co2 },
  { id: 15, name: 'CO₂ attenuation', unit: 'dB/m', scale: 1, color: '#674170', available: d => d.has_co2 },
  { id: 16, name: 'PM2.5 (ppm)', unit: 'ppm', scale: 1, color: '#8295a6', available: d => d.has_pm25 },
  { id: 17, name: 'PM2.5 particle concentration', unit: 'particles/m3', scale: 1, color: '#667b91', available: d => d.has_pm25 },
  { id: 18, name: 'PM2.5 activity concentration', unit: 'Bq/m3', scale: 1, color: '#536981', available: d => d.has_pm25 },
  { id: 19, name: 'PM2.5 attenuation', unit: 'dB/m', scale: 1, color: '#445a72', available: d => d.has_pm25 },
]
ChartJS.register(LineController, LineElement, LinearScale, PointElement, Tooltip)

function SensorChart({ device, metricId, range }: { device: Device | undefined; metricId: number; range: 'day' | 'week' | 'all' }) {
  const canvas = useRef<HTMLCanvasElement>(null)
  const metric = sensorMetrics.find(item => item.id === metricId)
  const historyId = device ? historyDeviceId(device) : ''
  const { samples, now, error, loading } = useHistory(historyId, range, true)
  const points = useMemo(() => samples.filter(sample => sample[2] === metricId).sort((a,b) => a[1]-b[1]), [samples, metricId])
  const start = range === 'all' ? (points[0]?.[1] ?? now) : now - historyRangeSeconds[range]
  const latest = points[points.length - 1]
  const historyMetric = metricId as HistoryMetric
  const unit = latest && metric ? historyUnit(historyMetric, latest) : metric?.unit ?? ''
  const unitPoints = useMemo(() => historyWindow(points, start, now).filter(point => historyUnit(historyMetric, point) === unit), [points, start, now, historyMetric, unit])
  useEffect(() => {
    if (!canvas.current || !unitPoints.length || !metric) return
    const discrete = historyDiscrete(historyMetric)
    const chart = new ChartJS(canvas.current, {
      type: 'line',
      data: { datasets: [{ label: metric.name, data: unitPoints.map(p => ({x:p[1], y:historyY(historyMetric, p)})), borderColor: metric.color, backgroundColor: metric.color, borderWidth: 2, stepped: discrete ? 'after' : false, pointRadius: 0 }] },
      options: { responsive: true, maintainAspectRatio: false, animation: false, parsing: false, plugins: { legend: { display: false }, tooltip: { callbacks: { title: items => items.length ? new Date(Number(items[0].parsed.x) * 1000).toLocaleString() : '', label: item => historyLabel(historyMetric, unitPoints[item.dataIndex]) } } }, scales: { x: { type: 'linear', min: start, max: now, ticks: { maxTicksLimit: 4, callback: value => new Date(Number(value)*1000).toLocaleTimeString([], {hour:'2-digit',minute:'2-digit'}) } }, y: { ticks: { maxTicksLimit: discrete ? 6 : 4, callback: value => { const tick = historyTick(historyMetric, Number(value)); return unit ? `${tick} ${unit}` : tick } } } } }
    })
    return () => chart.destroy()
  }, [points, unitPoints, metric, historyMetric, metricId, start, now, unit])
  const value = !latest || !metric ? '—' : historyLabel(historyMetric, latest)
  return <article className="dashboard-widget dashboard-chart" style={{ '--sensor-color': metric?.color ?? '#71859a' } as CSSProperties}><div className="dashboard-widget-title"><div><h3>{device?.name ?? 'Unavailable device'}</h3><span>{metric?.name ?? 'Sensor'} · {value}</span></div></div>{error && <p className="error">{error}</p>}{points.length ? <div className="dashboard-chart-canvas"><canvas ref={canvas} role="img" aria-label={`${device?.name} ${metric?.name} chart`} /></div> : loading ? <div className="dashboard-chart-loading" role="status" aria-label="Loading history"><span className="dashboard-chart-spinner" aria-hidden="true" /></div> : !error && <p className="muted small">No retained samples for this time period.</p>}</article>
}

function formatBytes(value: number | null | undefined): string {
  if (value === null || value === undefined) return '—'
  const units = ['B', 'KiB', 'MiB', 'GiB']
  let amount = value
  let unit = 0
  while (amount >= 1024 && unit < units.length - 1) { amount /= 1024; unit++ }
  return `${amount.toFixed(unit === 0 ? 0 : 1)} ${units[unit]}`
}

function GatewayWidget({ status, deviceCount, restarting, thread, resources }: { status: Status | null; deviceCount: number; restarting: boolean; thread: ThreadStatus | null; resources: ControllerStats | null }) {
  const coordinator = restarting ? 'Restarting' : status ? status.online ? 'Online' : 'Offline' : 'Checking'
  const network = status?.network_formed ? 'Formed' : status?.network_forming ? 'Forming' : 'Not formed'
  const threadNetwork = !thread ? 'Checking' : !thread.available ? 'Unavailable' : !thread.configured ? 'Not configured' : thread.attached ? 'Attached' : 'Waiting for attachment'
  const joining = status?.joining_preparing ? 'Preparing' : status?.joining_until && status.joining_until > Date.now() / 1000 ? 'Open' : 'Closed'
  return <article className="dashboard-widget gateway-widget"><h3>Gateway state</h3><dl className="gateway-widget-state"><div><dt>Coordinator</dt><dd className={status?.online && !restarting ? 'state-good' : 'state-bad'}>{coordinator}</dd></div><div><dt>Zigbee network</dt><dd>{network}</dd></div><div><dt>Matter network</dt><dd>{threadNetwork}</dd></div><div><dt>Joining</dt><dd>{joining}</dd></div><div><dt>Devices</dt><dd>{deviceCount}</dd></div><div><dt>Free memory</dt><dd>{formatBytes(resources?.memory_free_bytes)}</dd></div><div><dt>Free flash</dt><dd>{formatBytes(resources?.storage_free_bytes)}</dd></div></dl></article>
}

function GroupWidget({ group, devices, action }: { group: SceneGroupSummary | undefined; devices: Device[]; action: (url: string, body?: string) => void }) {
  if (!group) return <article className="dashboard-widget"><h3>Unavailable group</h3><p className="muted small">This saved group is no longer configured.</p></article>
  return <article className="dashboard-widget"><div className="dashboard-widget-title"><div><h3>{group.name}</h3><span>{group.members.length} light(s)</span></div><button className="button" onClick={() => action(`/api/scenes/${group.id}/off`)}>Turn off</button></div><div className="presets">{group.scenes.map(scene => <button className="button" key={scene.id} onClick={() => action(`/api/scenes/${group.id}/${scene.id}/activate`)}>▶ {scene.name}</button>)}</div><p className="muted small">Group members</p><div className="group-light-states">{group.members.map(address => { const d=devices.find(item=>item.ieee_address===address); return <span className="dashboard-member" key={address}>{d?.name ?? address} · {d ? (d.light_on ? 'On' : 'Off') : 'Unknown'}</span> })}</div></article>
}

function DeviceListWidget({ title, devices, empty }: { title: string; devices: Device[]; empty: string }) {
  return <article className="dashboard-widget"><div className="dashboard-widget-title"><div><h3>{title}</h3><span>{devices.length} device{devices.length === 1 ? '' : 's'}</span></div></div>{devices.length ? <ul className="dashboard-device-list">{devices.map(device => <li key={device.id}>{device.name}</li>)}</ul> : <p className="muted small">{empty}</p>}</article>
}

export function Dashboard({ config, editing, devices, groups, status, restarting, thread, resources, action, onConfig, onSettings, onDetails, onHistory, onConnection, onEdit, onCancel, onSave, saving, loaded, configError }: {
  config: DashboardConfig; editing: boolean; devices: Device[]; groups: SceneGroupSummary[]; status: Status | null; restarting: boolean; thread: ThreadStatus | null; resources: ControllerStats | null
  action: (url: string, body?: string) => void; onConfig: (value: DashboardConfig) => void
  onSettings: (device: Device) => void; onDetails: (device: Device) => void; onHistory: (device: Device) => void; onConnection: (device: Device) => void
  onEdit: () => void; onCancel: () => void; onSave: () => void; saving: boolean; loaded: boolean; configError: string
}) {
  const [scripts, setScripts] = useState<AutomationScript[]>([])
  useEffect(() => {
    let active = true
    const refresh = async () => {
      try { const result = await listAutomations(); if (active) setScripts(result.scripts) }
      catch { if (active) setScripts([]) }
    }
    void refresh()
    const timer = window.setInterval(() => void refresh(), 3000)
    return () => { active = false; window.clearInterval(timer) }
  }, [])
  const triggers = scripts.filter(script => script.enabled && script.loaded).flatMap(script => (script.triggers ?? []).map(trigger => ({ id: JSON.stringify([script.id, trigger.id]), name: `${trigger.label} (${script.id})`, script_id: script.id, trigger_id: trigger.id, label: trigger.label })))
  const [kind, setKind] = useState<DashboardWidget['kind']>('device')
  const [target, setTarget] = useState('')
  const [metric, setMetric] = useState(1)
  const pickedDevice = devices.find(device => device.id === target)
  const updateWidgets = (widgets: DashboardWidget[]) => onConfig({ ...config, widgets, gateway_widget_initialized: true })
  const add = () => {
    const targetless = kind === 'gateway' || kind === 'open_contacts' || kind === 'present_sensors'
    const widget: DashboardWidget = { id: `${Date.now()}-${Math.random().toString(36).slice(2)}`, kind, target: targetless ? kind : target, ...(kind === 'chart' ? { metric, range: 'day' as const } : {}) }
    if (kind === 'automation_trigger') {
      const trigger = triggers.find(item => item.id === target)
      if (!trigger) return
      widget.script_id = trigger.script_id; widget.trigger_id = trigger.trigger_id; widget.label = trigger.label
    }
    updateWidgets([...config.widgets, widget])
    setTarget('')
  }
  return <section className="dashboard-view">
    <div className="dashboard-heading"><div><p className="eyebrow">Your home</p><h2>Dashboard</h2></div>{editing ? <div className="toolbar"><button className="button" disabled={saving} onClick={onCancel}>Cancel</button><button className="button primary" disabled={saving} onClick={onSave}>{saving ? 'Saving…' : 'Save layout'}</button></div> : <button className="button" disabled={!loaded} onClick={onEdit}>{loaded ? 'Edit dashboard' : 'Loading…'}</button>}</div>
    {configError && <p className="error" role="alert">{configError}</p>}
    {editing && <div className="panel dashboard-editor"><strong>Add a widget</strong><div className="toolbar"><select value={kind} onChange={e => { const next=e.target.value as typeof kind; setKind(next); setTarget('') }}><option value="automation_trigger">Automation trigger</option><option value="gateway">Gateway state</option><option value="device">Device card</option><option value="group">Group card</option><option value="chart">Sensor chart</option><option value="open_contacts">Open contact sensors</option><option value="present_sensors">Present presence sensors</option></select>{kind !== 'gateway' && kind !== 'open_contacts' && kind !== 'present_sensors' && <select value={target} onChange={e => { const value=e.target.value; setTarget(value); if(kind==='chart'){ const selectedDevice=devices.find(d=>d.id===value); const first=sensorMetrics.find(m=>selectedDevice && m.available(selectedDevice)); if(first)setMetric(first.id) } }}><option value="">Choose {kind === 'automation_trigger' ? 'trigger' : kind === 'group' ? 'group' : 'device'}</option>{(kind === 'automation_trigger' ? triggers : kind === 'group' ? groups.map(g=>({id:String(g.id),name:g.name})) : devices.map(d=>({id:d.id,name:d.name}))).map(item=><option key={item.id} value={item.id}>{item.name}</option>)}</select>}{kind === 'chart' && <select value={metric} onChange={e=>setMetric(Number(e.target.value))}>{sensorMetrics.filter(m=>pickedDevice && m.available(pickedDevice)).map(m=><option key={m.id} value={m.id}>{m.name}</option>)}</select>}<button className="button primary" disabled={(kind !== 'gateway' && kind !== 'open_contacts' && kind !== 'present_sensors' && !target) || (kind==='chart' && !sensorMetrics.some(m=>m.id===metric && pickedDevice && m.available(pickedDevice))) || (kind==='gateway' && config.widgets.some(widget=>widget.kind==='gateway'))} onClick={add}>Add widget</button></div><p className="muted small">Use the arrows on widgets to change their order, then save the layout.</p></div>}
    <div className="dashboard-grid">{config.widgets.map((widget,index)=>{
      const move=(direction:number)=>{ const next=[...config.widgets]; const swap=index+direction; if(swap<0||swap>=next.length)return; [next[index],next[swap]]=[next[swap],next[index]]; updateWidgets(next) }
      const device=devices.find(d=>d.id===widget.target || d.endpoint_descriptors?.some(endpoint=>endpoint.id===widget.target))
      const controls=editing?<div className="dashboard-widget-actions"><button className="icon-button" aria-label="Move widget up" onClick={()=>move(-1)}>↑</button><button className="icon-button" aria-label="Move widget down" onClick={()=>move(1)}>↓</button><button className="icon-button" aria-label="Remove widget" onClick={()=>updateWidgets(config.widgets.filter(item=>item.id!==widget.id))}>×</button></div>:null
      if(widget.kind==='automation_trigger') {
        const trigger=triggers.find(item=>item.script_id===widget.script_id && item.trigger_id===widget.trigger_id)
        return <div className="dashboard-widget-shell" key={widget.id}>{controls}<article className="dashboard-widget"><h3>{trigger?.label ?? widget.label ?? widget.trigger_id ?? 'Automation trigger'}</h3><p className="muted small">{widget.script_id}</p><AutomationTrigger scriptId={widget.script_id ?? ''} triggerId={widget.trigger_id ?? ''} label={trigger?.label ?? widget.label ?? 'Run'} available={!!trigger} /></article></div>
      }
      if(widget.kind==='gateway')return <div className="dashboard-widget-shell" key={widget.id}>{controls}<GatewayWidget status={status} deviceCount={devices.length} restarting={restarting} thread={thread} resources={resources}/></div>
      if(widget.kind==='open_contacts')return <div className="dashboard-widget-shell" key={widget.id}>{controls}<DeviceListWidget title="Open contact sensors" empty="No open contact sensors." devices={devices.filter(d=>d.has_contact&&!d.has_water_leak&&!d.contact_trigger_mode&&d.contact_known&&d.contact_open)}/></div>
      if(widget.kind==='present_sensors')return <div className="dashboard-widget-shell" key={widget.id}>{controls}<DeviceListWidget title="Present presence sensors" empty="No present presence sensors." devices={devices.filter(d=>d.has_occupancy&&(d.kind==='presence'||d.kind==='occupancy')&&d.occupied)}/></div>
      if(widget.kind==='chart')return <div className="dashboard-widget-shell" key={widget.id}>{controls}<SensorChart device={device} metricId={widget.metric ?? 1} range={widget.range ?? 'day'}/>{editing&&<div className="dashboard-chart-settings"><label>Range <select value={widget.range??'day'} onChange={e=>updateWidgets(config.widgets.map(item=>item.id===widget.id?{...item,range:e.target.value as DashboardWidget['range']}:item))}><option value="day">24 hours</option><option value="week">7 days</option><option value="all">All retained</option></select></label><label>Value <select value={widget.metric??1} onChange={e=>updateWidgets(config.widgets.map(item=>item.id===widget.id?{...item,metric:Number(e.target.value)}:item))}>{sensorMetrics.filter(m=>device&&m.available(device)).map(m=><option key={m.id} value={m.id}>{m.name}</option>)}</select></label></div>}</div>
      if(widget.kind==='group')return <div className="dashboard-widget-shell" key={widget.id}>{controls}<GroupWidget group={groups.find(g=>String(g.id)===widget.target)} devices={devices} action={action}/></div>
      if(!device)return <div className="dashboard-widget-shell" key={widget.id}>{controls}<article className="dashboard-widget"><h3>Unavailable device</h3><p className="muted small">This saved device is no longer connected.</p></article></div>
      if(!status)return <div className="dashboard-widget-shell" key={widget.id}>{controls}<article className="dashboard-widget">Loading device…</article></div>
      const single={...status,devices:[device]}
      return <div className="dashboard-widget-shell" key={widget.id}>{controls}<DeviceGroups status={single} action={action} hideContext onSettings={()=>onSettings(device)} onDetails={()=>onDetails(device)} onHistory={()=>onHistory(device)} onConnection={()=>onConnection(device)}/></div>
    })}</div>
  </section>
}

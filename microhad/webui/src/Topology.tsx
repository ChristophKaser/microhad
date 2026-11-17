import { useEffect, useRef, useState } from 'react'
import { errorMessage, jsonRequest, post } from './api'
import type { Device, Status, TopologyResult } from './api'

const coordinator = 'coordinator'
const neighborId = (neighbor: TopologyResult['neighbors'][number]) => neighbor.node === 0 ? coordinator : neighbor.ieee_address

export function Topology({ devices, initial }: { devices: Device[]; initial: TopologyResult[] }) {
  const [results, setResults] = useState(initial)
  const [running, setRunning] = useState(false)
  const [progress, setProgress] = useState('')
  const [error, setError] = useState('')
  const cancelled = useRef(false)
  useEffect(() => { setResults(initial) }, [initial])
  useEffect(() => () => { cancelled.current = true }, [])
  const zigbee = devices.filter(d => d.source === 'zigbee' && d.ieee_address)
  const scan = async () => {
    cancelled.current = false; setRunning(true); setError(''); setProgress('Starting…')
    let index = 0; let done = 0
    const worker = async () => {
      while (!cancelled.current && index < zigbee.length) {
        const device = zigbee[index++]
        try {
          await post(`/api/devices/${encodeURIComponent(device.ieee_address)}/topology`)
          for (let i = 0; i < 35 && !cancelled.current; i++) {
            await new Promise(resolve => setTimeout(resolve, 1000))
            const status = await jsonRequest<Status>('/api/status')
            setResults(status.topology ?? [])
            const entry = status.topology?.find(t => t.ieee_address === device.ieee_address)
            if (entry && entry.neighbors_status !== 'waiting' && entry.routes_status !== 'waiting') break
          }
        } catch (e) { setError(errorMessage(e)) }
        done++; if (!cancelled.current) setProgress(`Checked ${done} of ${zigbee.length}`)
      }
    }
    await Promise.all(Array.from({ length: Math.min(4, zigbee.length) }, () => worker()))
    setRunning(false); if (!cancelled.current) setProgress(`Checked ${done} of ${zigbee.length}`)
  }
  const responding = results.filter(t => t.neighbors_status === 'complete' || t.routes_status === 'complete' || t.neighbors.length > 0 || t.routes.length > 0)
  const names = new Map(devices.map(d => [d.ieee_address, d.name || d.ieee_address]))
  names.set(coordinator, 'Coordinator')
  const respondedIds = new Set(responding.map(t => t.ieee_address))
  const childIds = new Set(responding.flatMap(t => t.neighbors.filter(n => n.relationship === 1 && n.node !== 0).map(n => n.ieee_address)))
  const ids = [...new Set([...respondedIds, ...childIds])].sort()
  const nodeIds = new Set([coordinator, ...ids])
  const points = new Map<string, { x: number; y: number }>([[coordinator, { x: 400, y: 270 }], ...ids.map((id, i): [string, { x: number; y: number }] => {
    const angle = 2 * Math.PI * i / Math.max(ids.length, 1) - Math.PI / 2
    return [id, { x: 400 + 290 * Math.cos(angle), y: 270 + 200 * Math.sin(angle) }]
  })])
  const label = (id: string) => names.get(id) ?? id
  return <section className="panel">
    <h2>Network topology</h2>
    <p>Arrows point from the device reporting a neighbor to that neighbor. The coordinator is shown at the center; outlined nodes are children reported by a router that did not themselves return a table. Lines describe reported neighbors, not the path taken by a command. Positions do not represent physical location.</p>
    <button className="button primary" disabled={running || !zigbee.length} onClick={() => void scan()}>Query Zigbee devices</button>{' '}
    {running && <button className="button" onClick={() => { cancelled.current = true; setProgress('Stopping…') }}>Stop</button>}
    <p role="status">{progress}</p>{error && <p role="alert">{error}</p>}
    {responding.length === 0 && <p>No topology responses yet. The coordinator is shown without inferred links.</p>}
    <svg viewBox="0 0 800 540" role="img" aria-label="Zigbee neighbor map" style={{ width: '100%', maxHeight: 600 }}>
      <defs><marker id="topology-arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M 0 0 L 10 5 L 0 10 z" fill="currentColor" /></marker></defs>
      {responding.flatMap(t => t.neighbors.filter(n => nodeIds.has(neighborId(n)) && neighborId(n) !== t.ieee_address).map((n, i) => {
        const target = neighborId(n); const a = points.get(t.ieee_address)!; const b = points.get(target)!
        const dx = b.x - a.x; const dy = b.y - a.y; const distance = Math.hypot(dx, dy)
        const ux = dx / distance; const uy = dy / distance
        // Curving to the left of each direction keeps reciprocal reports distinguishable.
        const start = { x: a.x + ux * 12, y: a.y + uy * 12 }; const end = { x: b.x - ux * 13, y: b.y - uy * 13 }
        const control = { x: (a.x + b.x) / 2 - uy * 20, y: (a.y + b.y) / 2 + ux * 20 }
        return <path key={`${t.ieee_address}-${target}-${i}`} d={`M ${start.x} ${start.y} Q ${control.x} ${control.y} ${end.x} ${end.y}`} fill="none" stroke="currentColor" opacity="0.55" markerEnd="url(#topology-arrow)"><title>{label(t.ieee_address)} reports {label(target)}{n.relationship === 1 ? ' as a child' : ''}; LQI {n.lqi}</title></path>
      }))}
      {[coordinator, ...ids].map(id => { const point = points.get(id)!; const indirect = id !== coordinator && !respondedIds.has(id); return <g key={id}><circle cx={point.x} cy={point.y} r="8" fill={indirect ? 'var(--panel, white)' : 'currentColor'} stroke="currentColor" strokeWidth="2" /><text x={point.x} y={point.y + 23} textAnchor="middle" fill="currentColor" fontSize="13">{label(id)}</text><title>{indirect ? 'Child reported by a router; no direct table response' : id === coordinator ? 'Zigbee coordinator' : 'Device returned topology data'}</title></g> })}
    </svg>
    {responding.map(t => <details key={t.ieee_address}><summary>{label(t.ieee_address)}</summary>
      <p>Neighbors: {t.neighbors_status}; routes: {t.routes_status}</p>
      <ul>{t.neighbors.filter(n => nodeIds.has(neighborId(n))).map((n, i) => <li key={`${neighborId(n)}-${i}`}>{label(t.ieee_address)} → {label(neighborId(n))}: LQI {n.lqi}{n.relationship === 1 ? ', child' : ''}</li>)}</ul>
      <ul>{t.routes.map((r, i) => <li key={i}>Destination 0x{r.destination.toString(16).padStart(4, '0')} via 0x{r.next_hop.toString(16).padStart(4, '0')} (status {r.status})</li>)}</ul>
    </details>)}
    <p>Matter topology is not available from the current Matter service.</p>
  </section>
}

import { useEffect, useRef, useState } from 'react'
import { errorMessage, jsonRequest, post } from './api'
import type { Device, Status, TopologyResult } from './api'

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
  const responding = results.filter(t => t.neighbors_status === 'complete' || t.routes_status === 'complete')
  const names = new Map(devices.map(d => [d.ieee_address, d.name || d.ieee_address]))
  const nodeIds = new Set(responding.map(t => t.ieee_address))
  const points = new Map(responding.map((t, i) => {
    const angle = 2 * Math.PI * i / Math.max(responding.length, 1) - Math.PI / 2
    return [t.ieee_address, { x: 400 + 290 * Math.cos(angle), y: 270 + 200 * Math.sin(angle) }]
  }))
  return <section className="panel">
    <h2>Network topology</h2>
    <p>Query Zigbee neighbor and routing tables. Only devices that responded appear on the map; links are reported neighbors, not proof of a route taken by a group command.</p>
    <button className="button primary" disabled={running || !zigbee.length} onClick={() => void scan()}>Query Zigbee devices</button>{' '}
    {running && <button className="button" onClick={() => { cancelled.current = true; setProgress('Stopping…') }}>Stop</button>}
    <p role="status">{progress}</p>{error && <p role="alert">{error}</p>}
    {responding.length === 0 ? <p>No topology responses yet.</p> : <svg viewBox="0 0 800 540" role="img" aria-label="Zigbee neighbor map" style={{ width: '100%', maxHeight: 600 }}>
      {responding.flatMap(t => t.neighbors.filter(n => nodeIds.has(n.ieee_address) && n.ieee_address !== t.ieee_address).map((n, i) => {
        const a = points.get(t.ieee_address)!; const b = points.get(n.ieee_address)!
        return <line key={`${t.ieee_address}-${n.ieee_address}-${i}`} x1={a.x} y1={a.y} x2={b.x} y2={b.y} stroke="currentColor" opacity="0.45"><title>Reported LQI {n.lqi}</title></line>
      }))}
      {responding.map(t => { const point = points.get(t.ieee_address)!; return <g key={t.ieee_address}><circle cx={point.x} cy={point.y} r="8" fill="currentColor" /><text x={point.x} y={point.y + 23} textAnchor="middle" fill="currentColor" fontSize="13">{names.get(t.ieee_address) ?? t.ieee_address}</text></g> })}
    </svg>}
    {responding.map(t => <details key={t.ieee_address}><summary>{names.get(t.ieee_address) ?? t.ieee_address}</summary>
      <p>Neighbors: {t.neighbors_status}; routes: {t.routes_status}</p>
      <ul>{t.neighbors.filter(n => nodeIds.has(n.ieee_address)).map((n, i) => <li key={`${n.ieee_address}-${i}`}>{names.get(n.ieee_address) ?? n.ieee_address}: LQI {n.lqi}, relationship {n.relationship}</li>)}</ul>
      <ul>{t.routes.map((r, i) => <li key={i}>Destination 0x{r.destination.toString(16).padStart(4, '0')} via 0x{r.next_hop.toString(16).padStart(4, '0')} (status {r.status})</li>)}</ul>
    </details>)}
    <p>Matter topology is not available from the current Matter service.</p>
  </section>
}

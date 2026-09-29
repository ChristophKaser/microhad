import { useEffect, useState } from 'react'
import { createRoot } from 'react-dom/client'
import type { Device } from './api'
import { errorMessage, form, jsonRequest, post } from './api'
import { DeviceGroups, DeviceSettings, LightGroupStates, deviceTypes, getDeviceType } from './devices'
import { Dashboard, type DashboardConfig } from './Dashboard'
import { HistoryDialog } from './HistoryDialog'
import { DeviceDetailsDialog, NetworkDialog, SceneTextDialog } from './DashboardDialogs'
import { SceneEditor } from './SceneEditor'
import { ShellyDialog } from './ShellyDevices'
import { PushDevices } from './PushDevices'
import { useGatewayData } from './useGatewayData'
import './style.css'

function LogLine({ line }: { line: string }) {
  const match = line.match(/^(\d+)\s+(SERIAL|EZSP|APS|FRAME)\b\s*(.*)$/)
  if (!match) return <span className="log-line">{line}</span>
  const [, epoch, tag, body] = match
  const time = new Date(Number(epoch) * 1000)
  const timestamp =
    time.toDateString() === new Date().toDateString()
      ? time.toLocaleTimeString()
      : time.toLocaleString()
  const ieee = tag === 'FRAME' ? body.match(/ieee=(0x[0-9a-f]+)/i)?.[1] : undefined
  const hue = ieee
    ? [...ieee].reduce((hash, char) => (hash * 31 + char.charCodeAt(0)) >>> 0, 0) % 360
    : undefined
  return (
    <span
      className={`log-line log-${tag.toLowerCase()}`}
      style={hue === undefined ? undefined : { color: `hsl(${hue} 72% 72%)` }}
    >
      <span className="log-time" title={`Unix timestamp: ${epoch}`}>
        {timestamp}
      </span>{' '}
      <b className="log-tag">{tag}</b> {body}
    </span>
  )
}

type GatewayAction = (url: string, body?: string) => Promise<void>

function JoinButton({
  compact,
  joinLeft,
  preparing,
  action,
}: {
  compact?: boolean
  joinLeft: number
  preparing: boolean
  action: GatewayAction
}) {
  let label = compact ? 'Allow join' : 'Allow joining (5 min)'
  if (joinLeft) label = compact ? 'Extend join' : 'Extend joining (5 min)'
  if (preparing) label = 'Preparing…'
  return (
    <button className="button primary" disabled={preparing} onClick={() => void action('/api/join')}>
      {label}
    </button>
  )
}

function App() {
  const { status, statusError, logs, logsError, refreshStatus } = useGatewayData()
  const [dialog, setDialog] = useState<'network' | 'scenes' | 'remotes' | 'text' | 'push' | null>(null)
  const [selected, setSelected] = useState<Device | null>(null)
  const [detailsDevice, setDetailsDevice] = useState<Device | null>(null)
  const [historyDevice, setHistoryDevice] = useState<Device | null>(null)
  const [restarting, setRestarting] = useState(false)
  const [shellyId, setShellyId] = useState<string | null>(null)
  const [tab, setTab] = useState('Dashboard')
  const visibleDeviceTypes = deviceTypes.filter(type => status?.devices.some(device => getDeviceType(device) === type.name))
  const visibleTypeNames = visibleDeviceTypes.map(type => type.name).join('|')
  useEffect(() => {
    if (deviceTypes.some(type => type.name === tab) && !visibleTypeNames.split('|').includes(tab)) setTab('Dashboard')
  }, [tab, visibleTypeNames])
  const [dashboard, setDashboard] = useState<DashboardConfig>({ widgets: [] })
  const [dashboardDraft, setDashboardDraft] = useState<DashboardConfig>({ widgets: [] })
  const [dashboardEditing, setDashboardEditing] = useState(false)
  const [dashboardSaving, setDashboardSaving] = useState(false)
  const [dashboardLoaded, setDashboardLoaded] = useState(false)
  const [dashboardError, setDashboardError] = useState('')
  useEffect(() => {
    let active = true
    void jsonRequest<DashboardConfig>('/api/dashboard/config').then(value => {
      if (!active) return
      const config = value && Array.isArray(value.widgets) ? value : { widgets: [] }
      setDashboard(config)
      setDashboardDraft(config)
      setDashboardLoaded(true)
    }).catch(error => { if (active) { setDashboardError(errorMessage(error)); setDashboardLoaded(true) } })
    return () => { active = false }
  }, [])
  const saveDashboard = async () => {
    setDashboardSaving(true)
    setDashboardError('')
    try {
      await jsonRequest('/api/dashboard/config', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(dashboardDraft) })
      setDashboard(dashboardDraft)
      setDashboardEditing(false)
    } catch (error) { setDashboardError(errorMessage(error)) }
    finally { setDashboardSaving(false) }
  }
  const action = async (url: string, body = '') => {
    try {
      const result = await post<{ message?: string }>(url, body)
      if (result?.message) window.alert(result.message)
      await refreshStatus()
    } catch (e) {
      window.alert(errorMessage(e))
    }
  }
  const restart = async () => {
    if (!window.confirm('Restart the Zigbee service? Joining will close briefly.')) return
    try {
      await post('/api/service/restart')
      setRestarting(true)
      window.setTimeout(() => {
        setRestarting(false)
        void refreshStatus()
      }, 3000)
    } catch (e) {
      window.alert(errorMessage(e))
    }
  }
  const enablePush = async () => {
    if (import.meta.env.DEV) {
      window.alert(
        'Browser push registration requires the gateway HTTPS dashboard. The local Vite server uses HTTP.'
      )
      return
    }
    if (!('serviceWorker' in navigator) || !('PushManager' in window)) {
      window.alert('Push is unavailable in this browser')
      return
    }
    try {
      const registration = await navigator.serviceWorker.register('/sw.js')
      const key = await jsonRequest<{ public_key: string }>('/api/push/vapid-key')
      const base64 = key.public_key
        .replace(/-/g, '+')
        .replace(/_/g, '/')
        .padEnd(Math.ceil(key.public_key.length / 4) * 4, '=')
      const bytes = Uint8Array.from(atob(base64), (char) => char.charCodeAt(0))
      const subscription = await registration.pushManager.subscribe({
        userVisibleOnly: true,
        applicationServerKey: bytes,
      })
      await post('/api/push/subscriptions', JSON.stringify(subscription))
      window.alert('Browser alerts enabled')
    } catch (e) {
      window.alert(`Could not enable alerts: ${errorMessage(e)}`)
    }
  }
  const now = Math.floor(Date.now() / 1000),
    joinLeft = Math.max(0, (status?.joining_until ?? 0) - now)
  const networkState = status?.network_formed
    ? 'Formed'
    : status?.network_forming
    ? 'Forming'
    : 'Not formed'
  const preparingJoin = !!status?.joining_preparing
  return (
    <main className="shell">
      <section className="mobile-top" aria-label="Gateway status">
        <div className="mobile-state">
          <strong>MicroHAD</strong>
          <span
            className={statusError || !status?.online ? 'state-bad' : 'state-good'}
            title={
              status
                ? `Serial RX: ${status.serial_rx_bytes} bytes · ${status.ash_rx_frames} frames`
                : statusError
            }
          >
            {restarting
              ? '● Restarting'
              : statusError
              ? '● Unavailable'
              : status?.online
              ? '● Online'
              : '● Offline'}
          </span>
          <span>⌘ {status ? networkState : 'Checking'}</span>
          <span>
            {joinLeft
              ? `Joining ${Math.ceil(joinLeft / 60)}m`
              : status?.joining_preparing
              ? 'Preparing join'
              : 'Join closed'}
          </span>
        </div>
      </section>
      <header className="hero">
        <div>
          <p className="eyebrow">Smart home / Gateway</p>
          <h1>MicroHAD</h1>
          <p className="hero-sub">Devices, scenes, and coordinator activity in one place.</p>
        </div>
        <div className="hero-actions"><div className="serial-state">
          {restarting
            ? 'Restarting service…'
            : statusError ||
              (status
                ? `Serial RX: ${status.serial_rx_bytes} bytes · ${status.ash_rx_frames} frames`
                : 'Connecting…')}
        </div></div>
      </header>
      <nav className="main-tabs" role="tablist" aria-label="Gateway views">
        {['Dashboard', 'Controls', 'Groups', ...visibleDeviceTypes.map(type => type.name), 'Diagnostics'].map(name => (
          <button key={name} role="tab" aria-selected={tab === name} className={`main-tab ${tab === name ? 'active' : ''}`} onClick={() => setTab(name)}>{name}</button>
        ))}
      </nav>
      {tab === 'Dashboard' && <Dashboard
        config={dashboardEditing ? dashboardDraft : dashboard}
        editing={dashboardEditing}
        devices={status?.devices ?? []}
        groups={status?.scene_groups ?? []}
        status={status}
        action={action}
        onConfig={setDashboardDraft}
        onSettings={setSelected}
        onDetails={setDetailsDevice}
        onHistory={setHistoryDevice}
        onConnection={(device) => setShellyId(device.external_id)}
        onEdit={() => { setDashboardDraft(dashboard); setDashboardEditing(true); setDashboardError('') }}
        onCancel={() => { setDashboardDraft(dashboard); setDashboardEditing(false); setDashboardError('') }}
        onSave={() => void saveDashboard()}
        saving={dashboardSaving}
        loaded={dashboardLoaded}
        configError={dashboardError}
      />}
      {tab === 'Controls' && <>
      <section className="overview">
        <div className={`overview-item ${statusError || !status?.online ? 'bad' : 'good'}`}>
          <span className="overview-icon">●</span>
          <div>
            <span className="overview-label">Coordinator</span>
            <strong>{statusError ? 'Unavailable' : status?.online ? 'Online' : 'Offline'}</strong>
          </div>
        </div>
        <div
          className={`overview-item ${
            status?.network_formed ? 'good' : status?.network_forming ? 'warn' : ''
          }`}
        >
          <span className="overview-icon">⌘</span>
          <div>
            <span className="overview-label">Zigbee network</span>
            <strong>{networkState}</strong>
          </div>
        </div>
        <div className="overview-item">
          <span className="overview-icon">▦</span>
          <div>
            <span className="overview-label">Devices</span>
            <strong>{status?.devices.length ?? '—'}</strong>
          </div>
        </div>
      </section>
      <section className="panel control-panel">
        <div className="section-heading">
          <div>
            <p className="eyebrow">Control center</p>
            <h2>Network & alerts</h2>
          </div>
          <p className="muted">
            {joinLeft
              ? `Joining enabled: pair a sensor within ${Math.ceil(joinLeft / 60)} minute(s).`
              : status?.joining_preparing
              ? 'Preparing secure pairing…'
              : 'Joining is closed.'}
          </p>
        </div>
        <div className="toolbar">
          <JoinButton joinLeft={joinLeft} preparing={preparingJoin} action={action} />
          <button
            className="button danger"
            onClick={() => setDialog('network')}
          >
            Create fresh network
          </button>
          <button className="button" onClick={restart}>
            Restart service
          </button>
          <button className="button" onClick={() => setShellyId('')}>Add Shelly device</button>
          <button className="button" onClick={() => setDialog('push')}>Push clients</button>
          <button className="button" disabled={!status?.notifications_enabled} onClick={() => void action('/api/notifications/test')}>Test alert</button>
        </div>
      </section>
      </>}
      {tab === 'Groups' && <section className="panel">
        <div className="section-heading">
          <div><p className="eyebrow">Lighting</p><h2>Scenes &amp; groups</h2></div>
          <div className="toolbar">
            <button className="button primary" onClick={() => setDialog('scenes')}>Configure scenes &amp; groups</button>
            <button className="button" onClick={() => setDialog('remotes')}>Configure remotes</button>
            <button className="button" onClick={() => setDialog('text')}>Advanced text editor</button>
          </div>
        </div>
        <p className="muted">Native-capable bulbs are recalled together with Zigbee Groups and Scenes; unsupported bulbs use a direct fallback.</p>
        <div className="scene-grid">
          {status?.scene_groups.length ? status.scene_groups.map(group => <article className="scene-summary" key={group.id}>
            <h3>{group.name}</h3><p className="muted small">Zigbee group {group.id} · {group.members.length} light(s)</p>
            <div className="presets"><LightGroupStates members={group.members} devices={status?.devices ?? []} />{group.scenes.map(scene => <button key={scene.id} className="button" onClick={() => action(`/api/scenes/${group.id}/${scene.id}/activate`)}>▶ {scene.name}</button>)}</div>
            <button className="button danger" onClick={() => action(`/api/scenes/${group.id}/off`)}>⏻ Turn group off</button>
          </article>) : <div className="empty">No scene groups configured yet.</div>}
        </div>
      </section>}
      {visibleDeviceTypes.some(type => type.name === tab) && <section className="content-section">
        <div className="section-heading">
          <div>
            <p className="eyebrow">Connected home</p>
            <h2>Devices</h2>
          </div>
        </div>
        <DeviceGroups
          status={status}
          action={action}
          onSettings={setSelected}
          onDetails={setDetailsDevice}
          onHistory={setHistoryDevice}
          onConnection={(device) => setShellyId(device.external_id)}
          onlyType={tab}
        />
      </section>
      }
      {tab === 'Diagnostics' && <section className="panel">
        <details open>
          <summary>
            <span className="eyebrow">Diagnostics</span>
            <h2>Live coordinator log</h2>
          </summary>
          <pre className="logs">
            {logsError
              ? `Unable to load logs: ${logsError}`
              : logs.map((line, index) => <LogLine key={index} line={line} />)}
          </pre>
        </details>
      </section>
      }
      {selected && (
        <DeviceSettings
          key={selected.ieee_address}
          device={selected}
          onClose={() => setSelected(null)}
          onSaved={() => void refreshStatus()}
        />
      )}
      {detailsDevice && (
        <DeviceDetailsDialog
          device={status?.devices.find((device) => device.id === detailsDevice.id) ?? detailsDevice}
          configuration={status?.device_configurations.find((item) => item.ieee_address === detailsDevice.ieee_address)}
          onClose={() => setDetailsDevice(null)}
        />
      )}
      {historyDevice && (
        <HistoryDialog key={historyDevice.id} device={historyDevice} onClose={() => setHistoryDevice(null)} />
      )}
      {shellyId !== null && <ShellyDialog key={shellyId || 'new'} id={shellyId} onClose={() => setShellyId(null)} onSaved={() => void refreshStatus()} />}
      {dialog === 'scenes' && (
        <SceneEditor
          devices={status?.devices ?? []}
          nativeStatuses={status?.native_bindings ?? []}
          mode="scenes"
          onClose={() => setDialog(null)}
          onSaved={() => void refreshStatus()}
        />
      )}
      {dialog === 'remotes' && (
        <SceneEditor
          devices={status?.devices ?? []}
          nativeStatuses={status?.native_bindings ?? []}
          mode="remotes"
          onClose={() => setDialog(null)}
          onSaved={() => void refreshStatus()}
        />
      )}
      {dialog === 'text' && (
        <SceneTextDialog
          onClose={() => setDialog(null)}
          onSaved={() => void refreshStatus()}
        />
      )}
      {dialog === 'network' && (
        <NetworkDialog
          onClose={() => setDialog(null)}
          onSaved={() => void refreshStatus()}
        />
      )}
      {dialog === 'push' && <PushDevices
        enabled={!!status?.notifications_enabled}
        onClose={() => setDialog(null)}
        onEnableBrowser={enablePush}
        onTest={() => void action('/api/notifications/test')}
        onToggle={(enabled) => void action('/api/notifications/enabled', form({ enabled: String(enabled) }))}
      />}
    </main>
  )
}

createRoot(document.getElementById('root')!).render(<App />)

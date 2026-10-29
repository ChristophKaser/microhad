import { AccessControls } from './AccessControls'
import { useEffect, useLayoutEffect, useRef, useState } from 'react'
import { createRoot } from 'react-dom/client'
import type { Device } from './api'
import { errorMessage, form, jsonRequest, post } from './api'
import { DeviceGroups, DeviceSettings, LightGroupStates, deviceTypes, getDeviceType } from './devices'
import { Dashboard, type DashboardConfig } from './Dashboard'
import { HistoryDialog } from './HistoryDialog'
import { DeviceDetailsDialog, NetworkDialog, SceneTextDialog } from './DashboardDialogs'
import { SceneEditor } from './SceneEditor'
import { ExternalDeviceDialog } from './ExternalDevices'
import { PushDevices } from './PushDevices'
import { ControllerStats } from './ControllerStats'
import { MatterPanel } from './MatterPanel'
import { useMatterDevices } from './useMatterDevices'
import { useGatewayData } from './useGatewayData'
import { useGatewayOverview } from './useGatewayOverview'
import { GatewaySettingsDialog } from './GatewaySettingsDialog'
import { DeleteDeviceDialog, IncompleteDevices } from './DeviceDeletion'
import { AutomationScripts } from './AutomationScripts'
import './style.css'

function LogLine({ line }: { line: string }) {
  const match = line.match(/^(\d+)\s+(.*)$/)
  if (!match) return <span className="log-line">{line}</span>
  const [, epoch, rawBody] = match
  const time = new Date(Number(epoch) * 1000)
  if (Number.isNaN(time.getTime())) return <span className="log-line">{line}</span>
  const timestamp = time.toLocaleString()
  const syslog = rawBody.match(/^(?:Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec)\s+\d{1,2}\s+\d{2}:\d{2}:\d{2}\s+(\S+\s+\S+\s+[^:]+:\s*)(.*)$/)
  const prefix = syslog?.[1] ?? ''
  const message = syslog?.[2] ?? rawBody
  const tagged = message.match(/^(SERIAL|EZSP|APS|FRAME)\b\s*(.*)$/)
  const tag = tagged?.[1]
  const body = tagged?.[2] ?? message
  const ieee = tag === 'FRAME' ? body.match(/ieee=(0x[0-9a-f]+)/i)?.[1] : undefined
  const hue = ieee
    ? [...ieee].reduce((hash, char) => (hash * 31 + char.charCodeAt(0)) >>> 0, 0) % 360
    : undefined
  return (
    <span
      className={`log-line${tag ? ` log-${tag.toLowerCase()}` : ''}`}
      style={hue === undefined ? undefined : { color: `hsl(${hue} 72% 72%)` }}
    >
      <span className="log-time" title={`Unix timestamp: ${epoch}`}>
        {timestamp}
      </span>{' '}
      {prefix}{tag && <><b className="log-tag">{tag}</b>{' '}</>}{body}
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
  const [tab, setTab] = useState('Dashboard')
  const [automationDirty, setAutomationDirty] = useState(false)
  const [gatewaySettingsOpen, setGatewaySettingsOpen] = useState(false)
  const [logFilter, setLogFilter] = useState('')
  const { status, statusError, logs, logsError, logsHasMore, loadOlderLogs, refreshStatus } = useGatewayData(tab === 'Diagnostics')
  const { thread, resources } = useGatewayOverview()
  const logPane = useRef<HTMLPreElement>(null)
  const followLogs = useRef(true)
  const preserveLogPosition = useRef<{ height: number; top: number } | null>(null)
  const filterTerms = logFilter.trim().toLocaleLowerCase().split(/\s+/).filter(Boolean)
  const visibleLogs = filterTerms.length
    ? logs.filter(line => {
      const normalized = line.toLocaleLowerCase()
      return filterTerms.every(term => normalized.includes(term))
    })
    : logs
  useLayoutEffect(() => {
    const pane = logPane.current
    if (!pane) return
    const previous = preserveLogPosition.current
    if (previous) {
      pane.scrollTop = previous.top + pane.scrollHeight - previous.height
      preserveLogPosition.current = null
    } else if (followLogs.current) pane.scrollTop = pane.scrollHeight
  }, [logs])
  const loadMoreLogs = async () => {
    const pane = logPane.current
    if (pane) preserveLogPosition.current = { height: pane.scrollHeight, top: pane.scrollTop }
    if (!await loadOlderLogs()) preserveLogPosition.current = null
  }
  const matterController = useMatterDevices()
  const catalogStatus = status
  const [dialog, setDialog] = useState<'network' | 'scenes' | 'remotes' | 'text' | 'push' | null>(null)
  const [deletingDevice, setDeletingDevice] = useState<Device | null>(null)
  const [selected, setSelected] = useState<Device | null>(null)
  const [detailsDevice, setDetailsDevice] = useState<Device | null>(null)
  const [historyDevice, setHistoryDevice] = useState<Device | null>(null)
  const [restarting, setRestarting] = useState(false)
  const [externalDeviceId, setExternalDeviceId] = useState<string | null>(null)
  const visibleDeviceTypes = deviceTypes.filter(type => catalogStatus?.devices.some(device => getDeviceType(device) === type.name))
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
  const prepareDashboard = (value: DashboardConfig): DashboardConfig => {
    const safe = value && Array.isArray(value.widgets) ? value : { widgets: [] }
    const widgets = [...safe.widgets]
    if (!safe.gateway_widget_initialized && !widgets.some(widget => widget.kind === 'gateway')) {
      widgets.unshift({ id: 'gateway-state', kind: 'gateway', target: 'gateway' })
    }
    return { ...safe, widgets, gateway_widget_initialized: true }
  }
  useEffect(() => {
    let active = true
    void jsonRequest<DashboardConfig>('/api/dashboard/config').then(value => {
      if (!active) return
      const config = prepareDashboard(value)
      setDashboard(config)
      setDashboardDraft(config)
      setDashboardLoaded(true)
    }).catch(error => { if (active) { const fallback = prepareDashboard({ widgets: [] }); setDashboard(fallback); setDashboardDraft(fallback); setDashboardError(errorMessage(error)); setDashboardLoaded(true) } })
    return () => { active = false }
  }, [])
  const afterDeviceDeletion = () => {
    setDeletingDevice(null)
    setSelected(null)
    void refreshStatus()
    void jsonRequest<DashboardConfig>('/api/dashboard/config').then(value => {
      setDashboard(value); setDashboardDraft(value)
    }).catch(error => setDashboardError(errorMessage(error)))
  }
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
      const deviceId = url.startsWith('/api/devices/') ? decodeURIComponent(url.slice('/api/devices/'.length).split('/')[0]) : ''
      const matterDevice = status?.devices.find(device => device.id === deviceId)
      const result: { message?: string } = matterDevice?.source === 'matter'
        ? await matterController.execute(matterDevice, url, body).then(() => ({}))
        : await post<{ message?: string }>(url, body)
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
          <strong className="mobile-brand"><img src="/microhad-icon.svg" width="24" height="24" alt="" />MicroHAD</strong>
          <span
            className={statusError || !status?.online ? 'state-bad' : 'state-good'}
            title={statusError}
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
          <h1 className="hero-brand"><img src="/microhad-icon.svg" width="56" height="56" alt="" />MicroHAD</h1>
          <p className="hero-sub">Devices, scenes, and coordinator activity in one place.</p>
        </div>
        <div className="hero-actions"><AccessControls /><div className="serial-state">
          {restarting ? 'Restarting service…' : statusError ? 'Gateway status unavailable' : status?.online ? 'Coordinator online' : 'Coordinator offline'}
        </div></div>
      </header>
      {gatewaySettingsOpen && <GatewaySettingsDialog onClose={() => setGatewaySettingsOpen(false)}
        matterNetworkConfigured={thread?.available ? thread.configured : undefined}
        onReplaceZigbeeNetwork={status?.network_formed ? () => { setGatewaySettingsOpen(false); setDialog('network') } : undefined} />}
      <nav className="main-tabs" role="tablist" aria-label="Gateway views">
        {['Dashboard', 'Gateway', 'Groups', 'Automations', ...visibleDeviceTypes.map(type => type.name), 'Diagnostics'].map(name => (
          <button key={name} role="tab" aria-selected={tab === name} className={`main-tab ${tab === name ? 'active' : ''}`} onClick={() => { if (name === tab || !automationDirty || window.confirm('Discard unsaved script changes?')) setTab(name) }}>{name}</button>
        ))}
      </nav>
      {tab === 'Dashboard' && <Dashboard
        config={dashboardEditing ? dashboardDraft : dashboard}
        editing={dashboardEditing}
        devices={catalogStatus?.devices ?? []}
        groups={status?.scene_groups ?? []}
        status={status}
        thread={thread}
        resources={resources}
        restarting={restarting}
        action={action}
        onConfig={setDashboardDraft}
        onSettings={setSelected}
        onDetails={setDetailsDevice}
        onHistory={setHistoryDevice}
        onConnection={(device) => setExternalDeviceId(device.source === 'fritz_box' ? `fritz:${device.external_id}` : device.external_id)}
        onEdit={() => { setDashboardDraft(dashboard); setDashboardEditing(true); setDashboardError('') }}
        onCancel={() => { setDashboardDraft(dashboard); setDashboardEditing(false); setDashboardError('') }}
        onSave={() => void saveDashboard()}
        saving={dashboardSaving}
        loaded={dashboardLoaded}
        configError={dashboardError}
      />}
      {tab === 'Automations' && <AutomationScripts onDirtyChange={setAutomationDirty} devices={catalogStatus?.devices ?? []} groups={status?.scene_groups ?? []} />}
      {tab === 'Gateway' && <>
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
        <div className={`overview-item ${thread?.attached ? 'good' : thread?.available && thread.configured ? 'warn' : ''}`}>
          <span className="overview-icon">◉</span>
          <div>
            <span className="overview-label">Matter network</span>
            <strong>{!thread ? 'Checking' : !thread.available ? 'Unavailable' : !thread.configured ? 'Not configured' : thread.attached ? 'Attached' : 'Waiting for attachment'}</strong>
          </div>
        </div>
        <div className="overview-item">
          <span className="overview-icon">▦</span>
          <div>
            <span className="overview-label">Devices</span>
            <strong>{catalogStatus?.devices.length ?? '—'}</strong>
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
          {status && !statusError && !status.network_formed && <button
            className="button danger"
            onClick={() => setDialog('network')}
          >
            Create Zigbee network
          </button>}
          <button className="button" onClick={restart}>
            Restart service
          </button>
          <button className="button" onClick={() => setExternalDeviceId('')}>Add external device</button>
          <button className="button" onClick={() => setDialog('push')}>Push clients</button>
          <button className="button" onClick={() => setGatewaySettingsOpen(true)}>Gateway settings</button>
          <button className="button" disabled={!status?.notifications_enabled} onClick={() => void action('/api/notifications/test')}>Test alert</button>
        </div>
      </section>
      <MatterPanel />
      <IncompleteDevices devices={catalogStatus?.devices ?? []} onDelete={setDeletingDevice} />
      <ControllerStats status={status} thread={thread} />
      <footer className="gateway-footer"><a href="/licenses.html">Open-source licenses</a></footer>
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
          status={catalogStatus}
          action={action}
          onSettings={setSelected}
          onDetails={setDetailsDevice}
          onHistory={setHistoryDevice}
          onConnection={(device) => setExternalDeviceId(device.source === 'fritz_box' ? `fritz:${device.external_id}` : device.external_id)}
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
          <label className="field log-filter"><span>Filter log lines</span><input type="search" value={logFilter} onChange={event => setLogFilter(event.target.value)} placeholder="Words separated by spaces (AND)" /></label>
          <pre className="logs" ref={logPane} onScroll={(event) => {
            const pane = event.currentTarget
            followLogs.current = pane.scrollHeight - pane.scrollTop - pane.clientHeight < 48
          }}>
            {logsError ? `Unable to load logs: ${logsError}` : <>
              {logsHasMore && <button className="button log-load-more" onClick={loadMoreLogs}>Load 500 more</button>}
              {visibleLogs.map((line, index) => <LogLine key={`${index}-${line}`} line={line} />)}
              {filterTerms.length > 0 && visibleLogs.length === 0 && <span className="muted">No log lines match these terms.</span>}
            </>}
          </pre>
        </details>
      </section>
      }
      {deletingDevice && <DeleteDeviceDialog device={deletingDevice} onClose={() => setDeletingDevice(null)} onDeleted={afterDeviceDeletion} />}
      {selected && (
        <DeviceSettings
          key={selected.ieee_address}
          device={selected}
          onClose={() => setSelected(null)}
          onDeleted={afterDeviceDeletion}
          onSaved={() => void refreshStatus()}
        />
      )}
      {detailsDevice && (
        <DeviceDetailsDialog
          device={catalogStatus?.devices.find((device) => device.id === detailsDevice.id) ?? detailsDevice}
          configuration={status?.device_configurations.find((item) => item.ieee_address === detailsDevice.ieee_address)}
          onClose={() => setDetailsDevice(null)}
        />
      )}
      {historyDevice && (
        <HistoryDialog key={historyDevice.id} device={historyDevice} onClose={() => setHistoryDevice(null)} />
      )}
      {externalDeviceId !== null && <ExternalDeviceDialog key={externalDeviceId || 'new'} id={externalDeviceId} onClose={() => setExternalDeviceId(null)} onSaved={() => void refreshStatus()} />}
      {dialog === 'scenes' && (
        <SceneEditor
          devices={status?.devices.filter(device => device.source === 'zigbee') ?? []}
          nativeStatuses={status?.native_bindings ?? []}
          mode="scenes"
          onClose={() => setDialog(null)}
          onSaved={() => void refreshStatus()}
        />
      )}
      {dialog === 'remotes' && (
        <SceneEditor
          devices={status?.devices.filter(device => device.source === 'zigbee') ?? []}
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
          existingNetwork={!!status?.network_formed}
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

import { useEffect, useState } from 'react'
import { createRoot } from 'react-dom/client'
import type { Device } from './api'
import { errorMessage, form, jsonRequest, post } from './api'
import { DeviceGroups, DeviceSettings, LightGroupStates } from './devices'
import { HistoryDialog } from './HistoryDialog'
import { DeviceDetailsDialog, NetworkDialog, SceneTextDialog } from './DashboardDialogs'
import { SceneEditor } from './SceneEditor'
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

function PushToggle({
  enabled,
  compact,
  action,
}: {
  enabled: boolean
  compact?: boolean
  action: GatewayAction
}) {
  return (
    <label className="check">
      <input
        type="checkbox"
        checked={enabled}
        onChange={(event) =>
          void action('/api/notifications/enabled', form({ enabled: String(event.target.checked) }))
        }
      />{' '}
      {compact ? 'Push' : 'Push notifications'}
    </label>
  )
}

function App() {
  const { status, statusError, logs, logsError, refreshStatus } = useGatewayData()
  const [dialog, setDialog] = useState<'network' | 'scenes' | 'remotes' | 'text' | null>(null)
  const [selected, setSelected] = useState<Device | null>(null)
  const [detailsDevice, setDetailsDevice] = useState<Device | null>(null)
  const [historyDevice, setHistoryDevice] = useState<Device | null>(null)
  const [restarting, setRestarting] = useState(false)
  const [mobileMenuOpen, setMobileMenuOpen] = useState(false)
  useEffect(() => {
    if (!mobileMenuOpen) return
    const closeOnEscape = (event: KeyboardEvent) => {
      if (event.key === 'Escape') setMobileMenuOpen(false)
    }
    document.addEventListener('keydown', closeOnEscape)
    return () => document.removeEventListener('keydown', closeOnEscape)
  }, [mobileMenuOpen])
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
      <section className="mobile-top" aria-label="Gateway status and controls">
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
        <div className="mobile-controls">
          <JoinButton compact joinLeft={joinLeft} preparing={preparingJoin} action={action} />
          <PushToggle compact enabled={!!status?.notifications_enabled} action={action} />
          <button className="button" onClick={enablePush}>
            Browser push
          </button>
          <button
            className="button"
            type="button"
            aria-expanded={mobileMenuOpen}
            aria-controls="mobile-overflow"
            onClick={() => setMobileMenuOpen((open) => !open)}
          >
            More ⋯
          </button>
        </div>
        {mobileMenuOpen && (
          <>
            <div className="mobile-menu-dismiss" onClick={() => setMobileMenuOpen(false)} />
            <nav
              className="mobile-overflow"
              id="mobile-overflow"
              aria-label="More gateway controls"
            >
              <button
                className="button danger"
                onClick={() => {
                  setMobileMenuOpen(false)
                  setDialog('network')
                }}
              >
                Create new network
              </button>
              <button
                className="button"
                onClick={() => {
                  setMobileMenuOpen(false)
                  void restart()
                }}
              >
                Restart service
              </button>
              <button
                className="button"
                disabled={!status?.notifications_enabled}
                onClick={() => {
                  setMobileMenuOpen(false)
                  void action('/api/notifications/test')
                }}
              >
                Test alert
              </button>
            </nav>
          </>
        )}
      </section>
      <header className="hero">
        <div>
          <p className="eyebrow">Smart home / Gateway</p>
          <h1>MicroHAD</h1>
          <p className="hero-sub">Devices, scenes, and coordinator activity in one place.</p>
        </div>
        <div className="serial-state">
          {restarting
            ? 'Restarting service…'
            : statusError ||
              (status
                ? `Serial RX: ${status.serial_rx_bytes} bytes · ${status.ash_rx_frames} frames`
                : 'Connecting…')}
        </div>
      </header>
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
            <span className="overview-label">Paired devices</span>
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
          <button className="button" onClick={enablePush}>
            Enable browser alerts
          </button>
          <button
            className="button"
            disabled={!status?.notifications_enabled}
            onClick={() => action('/api/notifications/test')}
          >
            Test alerts
          </button>
        </div>
        <div className="toggle-row">
          <PushToggle enabled={!!status?.notifications_enabled} action={action} />
          {import.meta.env.DEV && (
            <span className="muted small">
              Browser-push enrollment is disabled in local development.
            </span>
          )}
        </div>
      </section>
      <section className="content-section">
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
        />
      </section>
      <section className="panel">
        <div className="section-heading">
          <div>
            <p className="eyebrow">Lighting</p>
            <h2>Scenes & groups</h2>
          </div>
          <div className="toolbar">
            <button className="button primary" onClick={() => setDialog('scenes')}>
              Configure scenes & groups
            </button>
            <button className="button" onClick={() => setDialog('remotes')}>
              Configure remotes
            </button>
            <button className="button" onClick={() => setDialog('text')}>
              Advanced text editor
            </button>
          </div>
        </div>
        <p className="muted">
          Native-capable bulbs are recalled together with Zigbee Groups and Scenes; unsupported
          bulbs use a direct fallback.
        </p>
        <div className="scene-grid">
          {status?.scene_groups.length ? (
            status.scene_groups.map((group) => (
              <article className="scene-summary" key={group.id}>
                <h3>{group.name}</h3>
                <p className="muted small">
                  Zigbee group {group.id} · {group.members.length} light(s)
                </p>
                <div className="presets">
                  <LightGroupStates members={group.members} devices={status?.devices ?? []} />
                  {group.scenes.map((scene) => (
                    <button
                      key={scene.id}
                      className="button"
                      onClick={() => action(`/api/scenes/${group.id}/${scene.id}/activate`)}
                    >
                      ▶ {scene.name}
                    </button>
                  ))}
                </div>
                <button
                  className="button danger"
                  onClick={() => action(`/api/scenes/${group.id}/off`)}
                >
                  ⏻ Turn group off
                </button>
              </article>
            ))
          ) : (
            <div className="empty">No scene groups configured yet.</div>
          )}
        </div>
      </section>
      <section className="panel">
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
      {selected && (
        <DeviceSettings
          key={selected.ieee_address}
          device={selected}
          onClose={() => setSelected(null)}
          onSaved={() => void refreshStatus()}
        />
      )}
      {detailsDevice && (
        <DeviceDetailsDialog device={detailsDevice} onClose={() => setDetailsDevice(null)} />
      )}
      {historyDevice && (
        <HistoryDialog key={historyDevice.ieee_address} device={historyDevice} onClose={() => setHistoryDevice(null)} />
      )}
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
    </main>
  )
}

createRoot(document.getElementById('root')!).render(<App />)

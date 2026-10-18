import { useEffect, useRef, useState } from 'react'
import { matterAvailability, matterCall, sessionResult, terminalSession, validDataset, validSession } from './matter'
import type { MatterSession, MatterStatus, ThreadStatus } from './matter'
import { BrowserMatterRelay, chooseMatterDevice, matterQrCode, setupCode, webBluetoothAvailable, webQrScannerAvailable } from './browserMatter'

const phases: Record<string, string> = {
  waiting_ble: 'Waiting for phone BLE connection', pase: 'Checking setup code', provisioning: 'Providing Thread network',
  waiting_thread: 'Waiting for Thread attachment', case: 'Verifying connection over Thread', completing: 'Completing setup',
  complete: 'Device commissioned', cancelled: 'Cancelled', failed: 'Failed', timed_out: 'Timed out',
}
function failure(error: unknown): string {
  switch (error instanceof Error ? error.message : '') {
    case 'busy': return 'Another operation is active. Cancel its known session or wait for its deadline before retrying.'
    case 'stale_session': case 'not_found': return 'This session or job is no longer retained. Check the phone and start a new attempt explicitly.'
    case 'capacity': return 'Gateway queue is full. Wait, then retry explicitly.'
    case 'malformed': return 'Check the input and the Thread dataset channel. The gateway must use the current Zigbee channel.'
    case 'timeout': return 'The reply timed out. Check gateway status before retrying; a submitted operation may still be running.'
    case 'not_ready': return 'The Matter service or coordinator is not ready. Wait for it to reconnect.'
    default: return 'The gateway could not complete the request. Check its status and retry explicitly.'
  }
}

export function MatterPanel() {
  const [availability, setAvailability] = useState<{ matter: MatterStatus; thread: ThreadStatus } | null>(null)
  const [error, setError] = useState('')
  const [pending, setPending] = useState(false)
  const [dataset, setDataset] = useState('')
  const [sessionInput, setSessionInput] = useState('')
  const [scanning, setScanning] = useState(false)
  const [scannerMessage, setScannerMessage] = useState('')
  const [watch, setWatch] = useState('')
  const [session, setSession] = useState<MatterSession | null>(null)
  const [revision, setRevision] = useState(0)
  const operation = useRef<AbortController | null>(null)
  const browserRelay = useRef<BrowserMatterRelay | null>(null)
  const cameraStream = useRef<MediaStream | null>(null)
  const scannerFrame = useRef(0)
  const scannerActive = useRef(false)
  const scannerVideo = useRef<HTMLVideoElement | null>(null)
  const active = useRef(true)
  useEffect(() => {
    active.current = true
    const clear = () => { if (document.hidden) setDataset('') }
    document.addEventListener('visibilitychange', clear)
    return () => {
      active.current = false; operation.current?.abort(); browserRelay.current?.close(); scannerActive.current = false
      if (scannerFrame.current) window.cancelAnimationFrame(scannerFrame.current)
      cameraStream.current?.getTracks().forEach(track => track.stop()); cameraStream.current = null
      document.removeEventListener('visibilitychange', clear)
    }
  }, [])
  useEffect(() => {
    const controller = new AbortController()
    let timer = 0
    async function load() {
      try {
        const next = await matterAvailability(controller.signal)
        if (!controller.signal.aborted) setAvailability(next)
      } catch (cause) {
        if (!controller.signal.aborted) { setAvailability(null); setError(failure(cause)) }
      }
      if (!controller.signal.aborted) timer = window.setTimeout(() => void load(), 5000)
    }
    void load()
    return () => { controller.abort(); window.clearTimeout(timer) }
  }, [revision])
  useEffect(() => {
    if (!watch) return
    const controller = new AbortController()
    let timer = 0
    async function load() {
      try {
        const next = sessionResult(await matterCall('/api/matter/commissioning/status', { session: watch }, controller.signal))
        if (next.session !== watch) throw new Error('protocol')
        if (controller.signal.aborted) return
        setSession(next); setError('')
        if (!terminalSession(next)) timer = window.setTimeout(() => void load(), 2000)
      } catch (cause) { if (!controller.signal.aborted) { setWatch(''); setError(failure(cause)) } }
    }
    void load()
    return () => { controller.abort(); window.clearTimeout(timer) }
  }, [watch])
  async function mutate(route: string, fields: Record<string, unknown>) {
    if (operation.current) return
    const controller = new AbortController()
    operation.current = controller; setPending(true); setError('')
    // Stop status polling before cancellation; only one outstanding session call.
    if (route.endsWith('/cancel')) setWatch('')
    try {
      const result = await matterCall(route, fields, controller.signal)
      if (!active.current || controller.signal.aborted) return
      if (route.endsWith('/cancel')) {
        const next = sessionResult(result)
        if (next.session !== fields.session) throw new Error('protocol')
        setSession(next)
        if (!terminalSession(next)) setWatch(next.session)
      }
      setRevision(value => value + 1)
    } catch (cause) { if (active.current && !controller.signal.aborted) setError(failure(cause)) }
    finally { operation.current = null; if (active.current) setPending(false) }
  }
  async function commissionInBrowser(input: string) {
    if (operation.current) return
    try {
      const code = setupCode(input)
      if (!code.ble) throw new Error('unsupported')
      // The chooser must be called directly from this click handler.
      const device = await chooseMatterDevice()
      const controller = new AbortController(); operation.current = controller; setPending(true); setError('')
      const started = sessionResult(await matterCall('/api/matter/commissioning/start', {
        discriminator: code.discriminator, pin: code.pin, timeout_seconds: 120,
      }, controller.signal))
      setSession(started); setWatch(started.session)
      const relay = browserRelay.current = new BrowserMatterRelay(started.session, value => {
        if (!active.current) return
        setSession(value); if (terminalSession(value)) { setWatch(''); relay.close() }
      }, cause => {
        // The gateway cancels a failed relay asynchronously.  Continue polling
        // so the screen shows that terminal result rather than stale PASE state.
        if (active.current) setError(failure(cause))
      })
      await relay.connect(device)
    } catch (cause) { if (active.current) setError(failure(cause)) }
    finally { operation.current = null; if (active.current) setPending(false) }
  }
  function stopScanning() {
    scannerActive.current = false
    if (scannerFrame.current) window.cancelAnimationFrame(scannerFrame.current)
    scannerFrame.current = 0
    cameraStream.current?.getTracks().forEach(track => track.stop()); cameraStream.current = null
    if (scannerVideo.current) scannerVideo.current.srcObject = null
    setScanning(false)
  }
  async function startScanning() {
    if (!webQrScannerAvailable() || pending || scanning) return
    setScannerMessage('')
    try {
      const stream = await navigator.mediaDevices.getUserMedia({ audio: false, video: { facingMode: { ideal: 'environment' } } })
      if (!active.current) { stream.getTracks().forEach(track => track.stop()); return }
      cameraStream.current = stream; scannerActive.current = true; setScanning(true)
    } catch { if (active.current) setScannerMessage('Camera access was unavailable. Enter the Matter setup code instead.') }
  }
  useEffect(() => {
    if (!scanning || !scannerVideo.current || !cameraStream.current || !window.BarcodeDetector) return
    const video = scannerVideo.current
    const detector = new window.BarcodeDetector({ formats: ['qr_code'] })
    video.srcObject = cameraStream.current
    void video.play().catch(() => { if (active.current) { setScannerMessage('The camera preview could not start. Enter the Matter setup code instead.'); stopScanning() } })
    const detect = async () => {
      if (!scannerActive.current) return
      if (video.readyState >= HTMLMediaElement.HAVE_CURRENT_DATA) {
        try {
          const codes = await detector.detect(video)
          const value = codes.find(code => typeof code.rawValue === 'string' && matterQrCode(code.rawValue))?.rawValue
          if (value && scannerActive.current) { setSessionInput(value); setScannerMessage('Matter QR code scanned. Select Commission with this browser to continue.'); stopScanning() }
          else if (codes.length && scannerActive.current) setScannerMessage('That QR code is not a supported Matter setup code. Keep scanning.')
        } catch {
          if (scannerActive.current) { setScannerMessage('QR scanning failed. Enter the Matter setup code instead.'); stopScanning() }
        }
      }
      if (scannerActive.current) scannerFrame.current = window.requestAnimationFrame(detect)
    }
    scannerFrame.current = window.requestAnimationFrame(detect)
  }, [scanning])
  useEffect(() => { if (availability?.thread.configured) setDataset('') }, [availability?.thread.configured])
  if (availability && !availability.matter.enabled) return null
  const ready = availability?.matter.available && availability.matter.controller_initialized &&
    availability.matter.fabric_ready && availability.matter.state === 'running' && availability.thread.available &&
    ['detached', 'attaching', 'attached', 'error'].includes(availability.thread.state ?? '')
  const configured = availability?.thread.configured
  return <section className="panel matter-panel">
    <div className="section-heading"><div><p className="eyebrow">Matter over Thread</p><h2>Device setup</h2></div>
      <button className="button" disabled={pending} onClick={() => { setError(''); setRevision(value => value + 1) }}>Check gateway</button></div>
    <p role="status" aria-live="polite">{!availability ? error ? 'Matter status unavailable. Check the gateway connection.' : 'Checking Matter service…' : !ready ? 'Matter service unavailable or reconnecting.' : availability.thread.state === 'error' ? 'Thread radio apply failed. Retry after the coordinator is online.' : !configured ? 'Thread network has not been configured.' : availability.thread.attached ? 'Thread network attached.' : 'Thread network configured; waiting for attachment.'}</p>
    {error && <p className="error" role="alert">{error}</p>}
    {ready && configured === false && <div className="matter-network">
      <p>Create a Thread network, or import your existing network’s operational dataset. Keep the dataset private.</p>
      <div className="toolbar"><button className="button primary" disabled={pending} onClick={() => void mutate('/api/thread/create', {})}>Create Thread network</button></div>
      <form onSubmit={event => { event.preventDefault(); const value = dataset; setDataset(''); void mutate('/api/thread/import', { dataset: value }) }}>
        <label>Operational dataset (hex)<input type="password" autoComplete="off" spellCheck={false} maxLength={508} value={dataset} disabled={pending} onChange={event => setDataset(event.target.value)} /></label>
        <button className="button" disabled={pending || !validDataset(dataset)}>Import dataset</button>
      </form>
    </div>}
    {ready && configured && <>
      <p>Commission with <strong>Garage</strong>, or use this browser’s Bluetooth support. Both use the same gateway and commissioning sessions.</p>
      <p className="muted small">Gateway address: {window.location.protocol === 'http:' ? window.location.origin : `http://${window.location.hostname}`}</p>
      <form onSubmit={event => { event.preventDefault(); const input = sessionInput; setSessionInput(''); void commissionInBrowser(input) }}>
        <label>Matter setup code<input autoComplete="off" spellCheck={false} maxLength={512} value={sessionInput} disabled={pending || !webBluetoothAvailable()} onChange={event => setSessionInput(event.target.value)} /></label>
        <button className="button primary" disabled={pending || !sessionInput.trim() || !webBluetoothAvailable()}>Commission with this browser</button>
        {!webBluetoothAvailable() && <p className="muted small">Bluetooth commissioning needs Chrome or Edge on Android or desktop, opened over HTTPS. You can still use Garage below.</p>}
      </form>
      {webQrScannerAvailable() && <div className="matter-scanner">
        <button className="button" disabled={pending || !webBluetoothAvailable()} onClick={() => void startScanning()}>Scan Matter QR code</button>
        {scannerMessage && <p className="muted small" role="status">{scannerMessage}</p>}
      </div>}
      {scanning && <div className="modal matter-scanner-modal">
        <section className="matter-scanner-dialog" role="dialog" aria-modal="true" aria-labelledby="matter-scanner-title">
          <div className="matter-scanner-heading"><div><p className="eyebrow">Matter commissioning</p><h2 id="matter-scanner-title">Scan Matter QR code</h2></div>
            <button className="icon-button" onClick={stopScanning} aria-label="Close QR scanner">✕</button></div>
          <video ref={scannerVideo} muted playsInline aria-label="Matter QR code camera preview" />
          <p role="status">Point the camera at the accessory’s Matter QR code.</p>
          <button className="button" onClick={stopScanning}>Stop scanning</button>
        </section>
      </div>}
      <p className="muted small">Garage remains available: open <strong>Matter</strong>, enter this gateway’s address, and scan or enter the accessory’s setup code.</p>
      <form onSubmit={event => { event.preventDefault(); if (!validSession(sessionInput)) return; setSession(null); setError(''); setWatch(sessionInput); setSessionInput('') }}>
        <label>Garage commissioning session ID<input autoComplete="off" spellCheck={false} maxLength={16} value={sessionInput} disabled={pending} onChange={event => setSessionInput(event.target.value)} /></label>
        <button className="button" disabled={pending || !validSession(sessionInput)}>Watch Garage progress</button>
      </form>
      {session && <div aria-live="polite"><p><strong>{phases[session.state]}</strong>{session.deadline_remaining_seconds > 0 ? ` · ${session.deadline_remaining_seconds}s remaining` : ''}</p>
        {session.node && <p>Commissioned node: {session.node}</p>}
        {session.error && <p className="error">{failure(new Error(session.error))}</p>}
        {!terminalSession(session) && <button className="button danger" disabled={pending} onClick={() => void mutate('/api/matter/commissioning/cancel', { session: session.session })}>Cancel commissioning</button>}
      </div>}
    </>}
    {pending && <p role="status">Waiting for gateway reply…</p>}
    {ready && configured && availability.thread.state === 'error' && <button className="button" disabled={pending || !availability.thread.zigbee_channel} onClick={() => void mutate('/api/thread/retry', {})}>Retry stored Thread dataset</button>}
  </section>
}

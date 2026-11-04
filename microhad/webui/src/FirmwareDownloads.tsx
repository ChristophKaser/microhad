import { useEffect, useState } from 'react'
import { authFetch } from './auth'
import type { Device } from './api'
import { matchingFirmware, parseCatalog, type Catalog, type Firmware } from './firmwareCatalog'

const cache = new Map<string, { expires: number; promise: Promise<Firmware[]> }>()
async function readJson(url: string, authenticated = false): Promise<unknown> {
  const controller = new AbortController()
  const timeout = window.setTimeout(() => controller.abort(), authenticated ? 35000 : 8000)
  try {
    const response = await (authenticated ? authFetch : fetch)(url, { signal: controller.signal })
    if (!response.ok) throw new Error('Catalog unavailable')
    if (Number(response.headers.get('content-length')) > 4 * 1024 * 1024) throw new Error('Catalog too large')
    // Bound decoded data too, including servers that omit Content-Length.
    const reader = response.body?.getReader()
    if (!reader) throw new Error('Empty catalog')
    let length = 0
    const chunks: Uint8Array[] = []
    for (;;) {
      const { value, done } = await reader.read()
      if (done) break
      length += value.length
      if (length > 4 * 1024 * 1024) { await reader.cancel(); throw new Error('Catalog too large') }
      chunks.push(value)
    }
    const bytes = new Uint8Array(length)
    let offset = 0
    for (const chunk of chunks) { bytes.set(chunk, offset); offset += chunk.length }
    return JSON.parse(new TextDecoder().decode(bytes))
  } finally { window.clearTimeout(timeout) }
}
function load(catalog: Catalog): Promise<Firmware[]> {
  const key = `${catalog.id}:${catalog.url}`
  const previous = cache.get(key)
  if (previous && previous.expires > Date.now()) return previous.promise
  const promise = (async () => {
    if (!catalog.gateway_only) {
      try { return parseCatalog(catalog, await readJson(catalog.url)) } catch { /* CORS, mixed content, or network failure: use the restricted proxy. */ }
    }
    return parseCatalog(catalog, await readJson(`/api/ota/catalog/${encodeURIComponent(catalog.id)}`, true))
  })().catch(error => { cache.delete(key); throw error })
  cache.set(key, { expires: Date.now() + 3600000, promise })
  return promise
}
export function FirmwareDownloads({ device }: { device: Device }) {
  const [images, setImages] = useState<Firmware[]>([])
  const [failed, setFailed] = useState<string[]>([])
  const [loading, setLoading] = useState(true)
  const [retry, setRetry] = useState(0)
  useEffect(() => {
    let mounted = true
    setLoading(true); setFailed([]); setImages([])
    void (async () => {
      const response = await authFetch('/api/ota/catalogs')
      if (!response.ok) throw new Error('Firmware catalogs unavailable')
      const catalogs = await response.json() as Catalog[]
      const manufacturer = device.ota_manufacturer ?? device.node_descriptor?.manufacturer_code
      const applicable = catalogs.filter(catalog => catalog.manufacturer === undefined || catalog.manufacturer === manufacturer)
      // Sequential fallback requests keep the constrained gateway from being flooded.
      const found: Firmware[] = [], errors: string[] = []
      for (const catalog of applicable) {
        try { found.push(...await load(catalog)) } catch { errors.push(catalog.label) }
        if (!mounted) return
      }
      setImages(found); setFailed(errors)
    })().catch(() => { if (mounted) setFailed(['Firmware catalogs']) }).finally(() => { if (mounted) setLoading(false) })
    return () => { mounted = false }
  }, [device.id, device.ota_manufacturer, device.node_descriptor?.manufacturer_code, retry])
  const matches = matchingFirmware(device, images)
  return <div aria-label="Firmware downloads">
    {loading && <p className="muted" role="status">Checking firmware catalogs…</p>}
    {matches.length > 0 && <>
      <h4>Available firmware downloads</h4>
      <ul>{matches.map(image => <li key={`${image.imageType}:${image.version}`}>
        <a href={image.url} target="_blank" rel="noopener noreferrer">Download firmware</a>
        {' · '}{image.source} · Version {image.version}
        {image.size !== undefined && <> · {image.size.toLocaleString()} bytes</>}
        {device.ota_file_version != null && (image.version === device.ota_file_version ? ' · Installed version' : image.version < device.ota_file_version ? ' · Older version' : ' · Newer version')}
      </li>)}</ul>
      <p className="muted">Download the file in your browser, then select it below to upload and install. Manufacturer download containers may need extraction before uploading.</p>
    </>}
    {!loading && !matches.length && <p className="muted">{device.ota_image_type == null ? 'Firmware identification is pending. Wake the device and refresh its details to discover more matches.' : 'No matching firmware found in the available catalogs.'}</p>}
    {failed.length > 0 && <p role="status">Could not read: {failed.join(', ')}. <button type="button" onClick={() => setRetry(value => value + 1)}>Retry catalogs</button></p>}
  </div>
}

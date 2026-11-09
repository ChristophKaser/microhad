import { useEffect, useState } from 'react'
import { authFetch } from './auth'
import type { Device } from './api'
import { readJson } from './FirmwareDownloads'
import { firmwareUrl, type Catalog } from './firmwareCatalog'
import { matterSubscriptions } from './matter'

type Version = { version: number; label: string; url: string; channel: string; notes?: string }
const uint = (v: unknown, max = 0xffffffff): v is number => typeof v === 'number' && Number.isInteger(v) && v >= 0 && v <= max
const object = (v: unknown): Record<string, unknown> => v && typeof v === 'object' && !Array.isArray(v) ? v as Record<string, unknown> : {}

export function parseMatterVersion(data: unknown, vid: number, pid: number, installed: number, version: number, channel: string, hardware?: number | null): Version | undefined {
  const row = object(object(data).modelVersion)
  if (row.vid !== vid || row.pid !== pid || row.softwareVersion !== version || row.softwareVersionValid !== true || version <= installed) return
  if (typeof row.softwareVersionString !== 'string' || row.softwareVersionString.length > 64) return
  if (row.minApplicableSoftwareVersion !== undefined && (!uint(row.minApplicableSoftwareVersion) || installed < row.minApplicableSoftwareVersion)) return
  if (row.maxApplicableSoftwareVersion !== undefined && (!uint(row.maxApplicableSoftwareVersion) || installed > row.maxApplicableSoftwareVersion)) return
  if (row.minHardwareVersion !== undefined && (!uint(row.minHardwareVersion, 65535) || !uint(hardware, 65535) || hardware < row.minHardwareVersion)) return
  if (row.maxHardwareVersion !== undefined && (!uint(row.maxHardwareVersion, 65535) || !uint(hardware, 65535) || hardware > row.maxHardwareVersion)) return
  const url = firmwareUrl(row.otaUrl)
  if (!url) return
  return { version, label: row.softwareVersionString, url, channel, notes: firmwareUrl(row.releaseNotesUrl) }
}

const cache = new Map<string, { expires: number; promise: Promise<unknown> }>()
function fetchDcl(catalog: Catalog, suffix: string): Promise<unknown> {
  const key = `${catalog.id}:${catalog.url}/${suffix}`
  const previous = cache.get(key)
  if (previous && previous.expires > Date.now()) return previous.promise
  if (!previous && cache.size >= 64) cache.clear()
  const promise = (async () => {
    if (!catalog.gateway_only) {
      try { return await readJson(`${catalog.url}/${suffix}`) } catch { /* Use the restricted gateway proxy. */ }
    }
    return readJson(`/api/ota/catalog/${catalog.id}/${suffix}`, true)
  })().catch(error => { cache.delete(key); throw error })
  cache.set(key, { expires: Date.now() + 3600000, promise })
  return promise
}

export function MatterFirmwareDownloads({ device }: { device: Device }) {
  const [images, setImages] = useState<Version[]>([])
  const [failed, setFailed] = useState<string[]>([])
  const [loading, setLoading] = useState(false)
  const [retry, setRetry] = useState(0)
  const [refreshState, setRefreshState] = useState('')
  const vid = device.matter_vendor_id, pid = device.matter_product_id, installed = device.matter_software_version
  useEffect(() => {
    if (!uint(vid, 65535) || !uint(pid, 65535) || !uint(installed)) { setLoading(false); setImages([]); return }
    let active = true
    setLoading(true); setFailed([]); setImages([])
    void (async () => {
      const response = await authFetch('/api/ota/catalogs')
      if (!response.ok) throw new Error('Catalogs unavailable')
      const catalogs = (await response.json() as Catalog[]).filter(c => c.id === 'matter-dcl-production' || c.id === 'matter-dcl-testnet')
      if (!catalogs.length) throw new Error('No Matter catalogs configured')
      const results: Version[] = [], errors: string[] = []
      for (const catalog of catalogs) {
        try {
          const suffix = `${vid}/${pid}`
          const response = object(await fetchDcl(catalog, suffix))
          if (response.notFound === true) continue
          const list = object(response.modelVersions)
          if (list.vid !== vid || list.pid !== pid || !Array.isArray(list.softwareVersions) || list.softwareVersions.length > 128 || !list.softwareVersions.every((v: unknown) => uint(v))) throw new Error('Invalid DCL version list')
          for (const version of list.softwareVersions as number[]) {
            if (version <= installed) continue
            const item = parseMatterVersion(await fetchDcl(catalog, `${suffix}/${version}`), vid, pid, installed, version, catalog.id === 'matter-dcl-testnet' ? `${catalog.label} · Test release` : catalog.label, device.matter_hardware_version)
            if (item) results.push(item)
          }
        } catch { errors.push(catalog.label) }
        if (!active) return
      }
      if (active) { setImages(results.sort((a, b) => b.version - a.version)); setFailed(errors) }
    })().catch(() => { if (active) setFailed(['Firmware catalogs']) }).finally(() => { if (active) setLoading(false) })
    return () => { active = false }
  }, [vid, pid, installed, device.matter_hardware_version, retry])
  const refresh = () => {
    if (!device.matter_node) return
    setRefreshState('Requesting…')
    void matterSubscriptions(device.matter_node, true, new AbortController().signal)
      .then(() => setRefreshState('Refresh requested. Waiting for device details.'))
      .catch(() => setRefreshState('Could not refresh device details.'))
  }
  if (!uint(vid, 65535) || !uint(pid, 65535) || !uint(installed)) return <div>
    <p className="muted">Matter firmware identification is pending. Wake the device if needed.</p>
    {device.matter_node && <button type="button" disabled={refreshState === 'Requesting…'} onClick={refresh}>Refresh details and restore subscriptions</button>}
    {refreshState && <p role="status">{refreshState}</p>}
  </div>
  return <div aria-label="Firmware downloads">
    <p className="muted">Installed version: {device.matter_software_version_string || installed}</p>
    {device.matter_node && <button type="button" disabled={refreshState === 'Requesting…'} onClick={refresh}>Refresh details and restore subscriptions</button>}
    {refreshState && <p role="status">{refreshState}</p>}
    {loading && <p role="status">Checking firmware catalogs…</p>}
    {!loading && images.length > 0 && <><h4>Available firmware downloads</h4><ul>{images.map(image => <li key={`${image.channel}:${image.version}`}><a href={image.url} target="_blank" rel="noopener noreferrer">Download firmware</a> · {image.channel} · Version {image.label}{image.notes && <> · <a href={image.notes} target="_blank" rel="noopener noreferrer">Release notes</a></>}</li>)}</ul><p className="muted">Download the file in your browser, then select it below to upload and install.</p></>}
    {!loading && !images.length && !failed.length && <p className="muted">No matching firmware found in the available catalogs.</p>}
    {failed.length > 0 && <p role="status">Could not read: {failed.join(', ')}. <button type="button" onClick={() => setRetry(value => value + 1)}>Retry catalogs</button></p>}
  </div>
}

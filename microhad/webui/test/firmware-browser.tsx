import { createRoot } from 'react-dom/client'
import { FirmwareDownloads } from '../src/FirmwareDownloads'
import type { Device } from '../src/api'
const calls: string[] = []
let scenario = 'matching'
const provider = { id: 'sonoff', label: 'SONOFF', url: 'https://catalog.example.test/upgrade.json', manufacturer: 4742 }
window.fetch = async (input) => {
  const url = String(input); calls.push(url)
  if (url === '/api/ota/catalogs') return Response.json([provider])
  if (url === provider.url) throw new TypeError('CORS blocked')
  if (url === '/api/ota/catalog/sonoff') {
    if (scenario === 'error') return new Response('unavailable', { status: 502 })
    return Response.json([{ fw_manufacturer_id: 4742, fw_image_type: 8199, fw_file_version: 2, fw_filesize: 256, model_id: 'TRVZB', fw_binary_url: 'https://firmware.example.test/original.ota' }])
  }
  throw new Error(`Unexpected request ${url}`)
}
const root = createRoot(document.getElementById('root')!)
const device = { id: 'one', source: 'zigbee', manufacturer_name: 'SONOFF', model_id: 'TRVZB', ota_manufacturer: 4742, ota_image_type: 8199, ota_file_version: 1 } as Device
const wait = () => new Promise(resolve => setTimeout(resolve, 60))
function check(ok: unknown, message: string) { if (!ok) throw new Error(message) }
async function main() {
  root.render(<FirmwareDownloads device={device} />); await wait(); await wait()
  const link = document.querySelector('a')!
  check(link?.href === 'https://firmware.example.test/original.ota', 'direct original firmware URL')
  check(link.rel.includes('noopener') && link.target === '_blank', 'safe external link')
  link.focus(); check(document.activeElement === link, 'keyboard focus')
  check(calls.includes('/api/ota/catalog/sonoff'), 'metadata gateway fallback')
  check(!calls.some(url => url.includes('firmware.example.test')), 'no firmware fetch')
  check(document.body.textContent?.includes('Newer version'), 'version label')
  check(document.documentElement.scrollWidth <= window.innerWidth, 'mobile layout')
  root.render(<FirmwareDownloads device={{ ...device, model_id: 'unknown', ota_image_type: 1 }} />); await wait()
  check(!document.querySelector('a'), 'no button for unmatched device')
  root.render(<FirmwareDownloads device={{ ...device, id: 'two', ota_manufacturer: 1 }} />); await wait()
  check(!document.querySelector('a'), 'no catalog match for unknown manufacturer')
  scenario = 'error'
  // A distinct configured URL avoids the successful shared catalog cache.
  provider.url = 'https://catalog.example.test/changed.json'
  root.render(<FirmwareDownloads device={{ ...device, id: 'three' }} />); await wait(); await wait()
  check(document.body.textContent?.includes('Could not read: SONOFF'), 'failure distinguished from empty catalog')
  check(!document.querySelector('a'), 'no firmware link after catalog failure')
  document.getElementById('result')!.textContent = 'PASS firmware catalogs: direct links, matching, CORS fallback, errors, keyboard and mobile layout'
  document.getElementById('result')!.dataset.result = 'passed'
}
main().catch(error => { document.getElementById('result')!.textContent = `FAIL ${error.message}` })

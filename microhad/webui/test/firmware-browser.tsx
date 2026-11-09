import { createRoot } from 'react-dom/client'
import { FirmwareDownloads } from '../src/FirmwareDownloads'
import { MatterFirmwareDownloads } from '../src/MatterFirmwareDownloads'
import type { Device } from '../src/api'
const calls: string[] = []
let scenario = 'matching'
const provider = { id: 'sonoff', label: 'SONOFF', url: 'https://catalog.example.test/upgrade.json', manufacturer: 4742 }
window.fetch = async (input) => {
  const url = String(input); calls.push(url)
  if (url === '/api/ota/catalogs') return Response.json(scenario.startsWith('matter') ? [{ id: 'matter-dcl-testnet', label: 'Matter testnet (test release)', url: `https://ledger.example.test/${scenario}/dcl/model/versions` }] : [provider])
  if (url.startsWith('https://ledger.example.test/')) throw new TypeError('CORS blocked')
  if (url === '/api/ota/catalog/matter-dcl-testnet/4476/4096') {
    if (scenario === 'matter-error') return new Response('unavailable', { status: 502 })
    if (scenario === 'matter-empty') return Response.json({ notFound: true })
    return Response.json({ modelVersions: { vid: 4476, pid: 4096, softwareVersions: [17039372] } })
  }
  if (url === '/api/ota/catalog/matter-dcl-testnet/4476/4096/17039372') return Response.json({ modelVersion: { vid: 4476, pid: 4096, softwareVersion: 17039372, softwareVersionString: '1.4.12', softwareVersionValid: true, otaUrl: 'https://firmware.example.test/matter.ota', minApplicableSoftwareVersion: 17039360, maxApplicableSoftwareVersion: 17039371 } })
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
  scenario = 'matter'
  root.render(<MatterFirmwareDownloads device={{ ...device, source: 'matter', matter_vendor_id: 4476, matter_product_id: 4096, matter_software_version: 17039366, matter_software_version_string: '1.4.6' }} />); await wait(); await wait(); await wait()
  check(document.querySelector('a')?.href === 'https://firmware.example.test/matter.ota', 'Matter direct image link')
  check(document.body.textContent?.includes('test release') && document.body.textContent?.includes('1.4.12'), 'Matter testnet label and version')
  check(calls.includes('/api/ota/catalog/matter-dcl-testnet/4476/4096/17039372'), 'Matter gateway fallback')
  check(!calls.some(url => url === 'https://firmware.example.test/matter.ota'), 'no Matter firmware fetch')
  scenario = 'matter-empty'
  root.render(<MatterFirmwareDownloads device={{ ...device, source: 'matter', matter_vendor_id: 4476, matter_product_id: 4096, matter_software_version: 17039365 }} />); await wait(); await wait(); await wait()
  check(document.body.textContent?.includes('No matching firmware'), 'Matter empty catalog distinguished')
  scenario = 'matter-error'
  root.render(<MatterFirmwareDownloads device={{ ...device, source: 'matter', matter_vendor_id: 4476, matter_product_id: 4096, matter_software_version: 17039364 }} />); await wait(); await wait(); await wait()
  check(document.body.textContent?.includes('Could not read:'), 'Matter catalog failure distinguished')
  root.render(<MatterFirmwareDownloads device={{ ...device, source: 'matter', matter_vendor_id: null }} />); await wait()
  check(document.body.textContent?.includes('identification is pending'), 'Matter missing identity distinguished')
  document.getElementById('result')!.textContent = 'PASS firmware catalogs: direct links, matching, CORS fallback, errors, keyboard and mobile layout'
  document.getElementById('result')!.dataset.result = 'passed'
}
main().catch(error => { document.getElementById('result')!.textContent = `FAIL ${error.message}` })

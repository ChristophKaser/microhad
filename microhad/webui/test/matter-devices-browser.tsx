import React, { useEffect, useState } from 'react'
import { createRoot } from 'react-dom/client'
import type { Device, Status } from '../src/api'
import { DeviceGroups, DeviceSettings } from '../src/devices'
import { MatterPanel } from '../src/MatterPanel'
import { useMatterDevices } from '../src/useMatterDevices'

function Host() {
  const controller = useMatterDevices()
  const [settingsDevice, setSettingsDevice] = useState<Device | null>(null)
  const [status, setStatus] = useState<Status>({ devices: [], scene_groups: [] } as unknown as Status)
  const refresh = async () => setStatus(await (await fetch('/api/status')).json())
  useEffect(() => { void refresh() }, [])
  async function action(url: string, body = '') {
    const id = decodeURIComponent(url.slice('/api/devices/'.length).split('/')[0])
    const device = status.devices.find(item => item.id === id)
    if (device) { await controller.execute(device, url, body); await refresh() }
  }
  return <>
    <DeviceGroups status={status} action={(url, body) => { void action(url, body) }}
      onSettings={setSettingsDevice}
      onDetails={() => {}} onHistory={() => {}} onConnection={() => {}} />
    <MatterPanel />
    {settingsDevice && <DeviceSettings device={settingsDevice} onClose={() => setSettingsDevice(null)} onSaved={() => {}} />}
  </>
}

const root = createRoot(document.getElementById('root')!)
root.render(<Host />)
const delay = (ms: number) => new Promise(resolve => setTimeout(resolve, ms))
function check(value: unknown, message: string) { if (!value) throw new Error(message) }
function button(label: string): HTMLButtonElement {
  const value = [...document.querySelectorAll('button')].find(item => item.textContent?.trim() === label)
  check(value, `missing button ${label}`)
  return value!
}
async function wait(test: () => boolean | Promise<boolean>, message: string) {
  for (let index = 0; index < 500; index++) { if (await test()) return; await delay(20) }
  throw new Error(`timed out: ${message}`)
}
async function run() {
  await wait(() => document.body.textContent?.includes('Matter light') === true, 'Matter endpoint in shared device catalog')
  check(document.body.textContent?.includes('Device setup'), 'Matter join/settings panel missing')
  check(!document.body.textContent?.includes('Matter devices'), 'separate Matter device list remains in setup UI')
  check(![...document.querySelectorAll('button')].some(item => item.textContent?.trim() === 'Refresh devices'), 'Matter-specific device list controls remain')
  check(document.body.textContent?.includes('Unknown'), 'unknown initial Matter values were not kept explicit')
  check(document.querySelector('[aria-label^="Settings for Matter light"]'), 'common device settings are missing from Matter card')
  check(document.querySelector('[aria-label^="History for Matter light"]'), 'common telemetry history is missing from Matter card')

  button('Turn on').click()
  await wait(() => document.body.textContent?.includes('Turn off') === true, 'confirmed OnOff state through common light card')
  button('75%').click()
  await wait(() => [...document.querySelectorAll('.brightness-row')].some(row => row.textContent?.includes('75%')),
    'confirmed Level value through common brightness control')
  const refresh = document.querySelector<HTMLButtonElement>('button[title="Refresh attributes"]')
  check(refresh, 'shared catalog refresh action missing')
  refresh!.click()
  await wait(() => document.body.textContent?.includes('23.15') && document.body.textContent?.includes('52.34') &&
    document.body.textContent?.includes('758.5 ppm') && document.body.textContent?.includes('12.75 ug/m3') &&
    document.body.textContent?.includes('Air quality: moderate'), 'typed sensor reads shown in shared device card')
  check(document.body.textContent?.includes('Temperature') && document.body.textContent?.includes('Humidity'), 'shared climate fields were not used')
  document.querySelector<HTMLButtonElement>('[aria-label^="Settings for Matter light"]')!.click()
  await wait(() => document.body.textContent?.includes('CO₂ alerts (ppm)') && document.body.textContent?.includes('PM2.5 alerts (ug/m3)'),
    'shared settings show unit-aware Matter concentration thresholds')
  check(document.body.textContent?.includes('Device settings'), 'common device settings did not open')
  await fetch('/fixture/mode', { method: 'POST', body: 'thread-error' })
  button('Check gateway').click()
  await wait(() => document.body.textContent?.includes('Thread radio apply failed'), 'Thread apply failure is actionable')
  button('Retry stored Thread dataset').click()
  await wait(async () => (await (await fetch('/fixture/retry-count')).json()).retryPosts === 1,
    'retry stored Thread dataset is sent without dataset input')
  document.body.dataset.result = 'passed'
  document.getElementById('result')!.textContent = 'PASS Matter endpoints render and operate through the shared device catalog, common controls, Matter join/settings and Thread dataset retry'
}
void run().catch(error => {
  document.body.dataset.result = 'failed'
  document.getElementById('result')!.textContent = String(error)
})

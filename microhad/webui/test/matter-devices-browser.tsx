import React from 'react'
import { createRoot } from 'react-dom/client'
import type { Status } from '../src/api'
import { DeviceGroups } from '../src/devices'
import { MatterPanel } from '../src/MatterPanel'
import { useMatterDevices } from '../src/useMatterDevices'

function Host() {
  const controller = useMatterDevices()
  const status = { devices: controller.devices, scene_groups: [] } as unknown as Status
  async function action(url: string, body = '') {
    const id = decodeURIComponent(url.slice('/api/devices/'.length).split('/')[0])
    const device = controller.devices.find(item => item.id === id)
    if (device) await controller.execute(device, url, body)
  }
  return <>
    <DeviceGroups status={status} action={(url, body) => { void action(url, body) }}
      onSettings={() => { throw new Error('Matter device exposed protocol-specific settings') }}
      onDetails={() => {}} onHistory={() => {}} onConnection={() => {}} />
    <MatterPanel />
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
async function wait(test: () => boolean, message: string) {
  for (let index = 0; index < 500; index++) { if (test()) return; await delay(20) }
  throw new Error(`timed out: ${message}`)
}
async function run() {
  await wait(() => document.body.textContent?.includes('Matter light') === true, 'Matter endpoint in shared device catalog')
  check(document.body.textContent?.includes('Device setup'), 'Matter join/settings panel missing')
  check(!document.body.textContent?.includes('Matter devices'), 'separate Matter device list remains in setup UI')
  check(![...document.querySelectorAll('button')].some(item => item.textContent?.trim() === 'Refresh devices'), 'Matter-specific device list controls remain')
  check(document.body.textContent?.includes('Unknown'), 'unknown initial Matter values were not kept explicit')

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
  document.body.dataset.result = 'passed'
  document.getElementById('result')!.textContent = 'PASS Matter endpoints render and operate through the shared device catalog, common controls and Matter join/settings area'
}
void run().catch(error => {
  document.body.dataset.result = 'failed'
  document.getElementById('result')!.textContent = String(error)
})

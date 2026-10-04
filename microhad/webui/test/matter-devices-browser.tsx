import React, { useState } from 'react'
import { createRoot } from 'react-dom/client'
import { MatterDevices } from '../src/MatterDevices'

let setAvailable: (value: boolean) => void
let unmount: () => void
let stage = 'mount'
function Host() {
  const [available, update] = useState(true)
  setAvailable = update
  return <MatterDevices available={available} />
}
const root = createRoot(document.getElementById('root')!)
unmount = () => root.unmount()
root.render(<Host />)
const delay = (ms: number) => new Promise(resolve => setTimeout(resolve, ms))
function check(value: unknown, message: string) { if (!value) throw new Error(message) }
function button(label: string): HTMLButtonElement {
  const value = [...document.querySelectorAll('button')].find(item => item.textContent === label)
  check(value, `missing button ${label}`)
  return value!
}
async function wait(checkValue: () => boolean) {
  for (let index = 0; index < 100; index++) { if (checkValue()) return; await delay(20) }
  throw new Error(`rendered condition timed out: ${stage}`)
}
async function mode(value: string) {
  stage = value
  await fetch('/fixture/mode', { method: 'POST', body: value })
}
async function count() { return (await (await fetch('/fixture/count')).json()).posts as number }
async function tests() {
  await wait(() => Boolean(document.querySelector('button')))
  check(await count() === 0, 'initial mount requested snapshots')
  await mode('normal')
  button('Refresh devices').click()
  await wait(() => document.querySelectorAll('li').length === 1)
  check(document.body.textContent?.includes('Endpoint 0'), 'root endpoint missing')
  button('Load next endpoint').click()
  await wait(() => document.querySelectorAll('li').length === 2)
  check(document.body.textContent?.includes('Advertised features: Power'), 'feature label missing')
  check(![...document.querySelectorAll('button')].some(item => item.textContent === 'Load next endpoint'), 'terminal page retained next button')
  check(await count() === 2, 'pagination submitted extra calls')
  button('Read power').click()
  await wait(() => document.body.textContent?.includes('Power: Off'))
  button('Turn on').click()
  await wait(() => document.body.textContent?.includes('Power: On'))
  button('Toggle').click()
  await wait(() => document.body.textContent?.includes('Power: Off'))
  check(await count() === 2, 'device commands caused unexpected snapshot calls')
  await mode('normal')
  button('Refresh devices').click()
  await wait(() => document.querySelectorAll('li').length === 1)
  await mode('stale')
  button('Load next endpoint').click()
  await wait(() => Boolean(document.querySelector('[role=alert]')))
  check(document.body.textContent?.includes('The device list changed'), 'stale revision message missing')
  check(button('Load next endpoint').disabled, 'stale revision allowed more pages')
  check(document.querySelectorAll('li').length === 1 && document.body.textContent?.includes('Previously loaded information'), 'stale page did not retain labelled cached rows')
  await delay(100)
  check(await count() === 1, 'stale revision automatically replayed')
  await mode('recovery')
  button('Refresh devices').click()
  await wait(() => Boolean(document.body.textContent?.includes('Setup needs recovery')))
  check(document.querySelectorAll('li').length === 1, 'refresh retained old endpoints')
  await mode('offline')
  button('Refresh devices').click()
  await wait(() => Boolean(document.body.textContent?.includes('Offline')))
  check(document.querySelectorAll('li').length === 1, 'offline snapshot lost identity')
  await mode('duplicate')
  button('Refresh devices').click()
  await wait(() => !document.body.textContent?.includes('Offline') && Boolean([...document.querySelectorAll('button')].find(item => item.textContent === 'Load next endpoint' && !item.disabled)))
  button('Load next endpoint').click()
  await wait(() => Boolean(document.querySelector('[role=alert]')))
  check(document.querySelectorAll('li').length === 1, 'duplicate endpoint merged')
  await mode('empty')
  button('Refresh devices').click()
  await wait(() => Boolean(document.body.textContent?.includes('No Matter devices have been recorded')))
  check(document.querySelectorAll('li').length === 0, 'empty refresh kept stale rows')
  setAvailable(false)
  await wait(() => button('Refresh devices').disabled)
  const before = await count()
  button('Refresh devices').click()
  await delay(50)
  check(await count() === before, 'unavailable view submitted a request')
  setAvailable(true)
  await wait(() => !button('Refresh devices').disabled)
  await mode('pending')
  button('Refresh devices').click()
  await wait(() => button('Refresh devices').disabled)
  setAvailable(false)
  await wait(() => Boolean(document.body.textContent?.includes('Gateway unavailable')))
  setAvailable(true)
  await wait(() => !button('Refresh devices').disabled)
  await mode('normal')
  button('Refresh devices').click()
  await wait(() => document.querySelectorAll('li').length === 1)
  await mode('pending')
  button('Refresh devices').click()
  await wait(() => button('Refresh devices').disabled)
  unmount()
  await delay(100)
  check(!document.getElementById('root')!.textContent, 'unmounted request updated UI')
  document.body.dataset.result = 'passed'
  document.getElementById('result')!.textContent = 'PASS rendered device refresh, pagination, confirmed OnOff read/control, revision loss, recovery, duplicates, empty state, availability and unmount'
}
void tests().catch(error => {
  document.body.dataset.result = 'failed'
  document.getElementById('result')!.textContent = String(error)
})

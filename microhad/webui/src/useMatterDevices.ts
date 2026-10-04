import { useCallback, useEffect, useRef, useState } from 'react'
import type { Device } from './api'
import { errorMessage } from './api'
import { matterAvailability, matterDeviceModel, matterDevicePage, matterLevel, matterOnOff, matterSensor } from './matter'
import type { MatterDevice, MatterDeviceState } from './matter'

const readClusters = [6, 8, 1026, 1029, 1037, 1066, 91]

export function useMatterDevices() {
  const [devices, setDevices] = useState<Device[]>([])
  const [controllerAvailable, setControllerAvailable] = useState(false)
  const [revision, setRevision] = useState(0)
  const cursor = useRef(0)
  const snapshotRevision = useRef<string>()
  const building = useRef(new Map<string, MatterDevice>())
  const states = useRef(new Map<string, MatterDeviceState>())
  const pending = useRef(false)
  const nextPoll = useRef(0)
  const statusCheckedAt = useRef(0)
  const matterReady = useRef(false)
  const active = useRef(true)

  const storeState = useCallback((id: string, update: Partial<MatterDeviceState>) => {
    const current = states.current.get(id) ?? {}
    const next = { ...current, ...update }
    states.current.set(id, next)
    const endpoint = building.current.get(id)
    if (endpoint) {
      const model = matterDeviceModel(endpoint, next)
      if (model) setDevices(currentDevices => currentDevices.map(device => device.id === id ? model : device))
    }
  }, [])

  const poll = useCallback(async () => {
    if (pending.current || Date.now() < nextPoll.current) return
    pending.current = true
    nextPoll.current = Date.now() + 1500
    const controller = new AbortController()
    try {
      if (Date.now() - statusCheckedAt.current >= 30000) {
        const availability = await matterAvailability(controller.signal)
        statusCheckedAt.current = Date.now()
        matterReady.current = availability.matter.enabled && availability.matter.available &&
          availability.matter.controller_initialized === true
        setControllerAvailable(matterReady.current)
        setDevices(current => current.map(device => ({ ...device, controller_available: matterReady.current })))
      }
      if (!matterReady.current) { nextPoll.current = Date.now() + 30000; return }
      const page = await matterDevicePage(cursor.current, snapshotRevision.current, controller.signal)
      if (!active.current) return
      if (cursor.current === 0) building.current = new Map()
      for (const endpoint of page.devices) {
        if (!endpoint.id || endpoint.endpoint === null) continue
        building.current.set(endpoint.id, endpoint)
      }
      if (page.next_cursor === null) {
        const nextDevices = [...building.current.values()].map(endpoint => matterDeviceModel(endpoint, states.current.get(endpoint.id!), matterReady.current))
          .filter((device): device is Device => device !== null)
        setDevices(nextDevices)
        for (const id of states.current.keys()) if (!building.current.has(id)) states.current.delete(id)
        cursor.current = 0
        snapshotRevision.current = undefined
        nextPoll.current = Date.now() + 15000
      } else {
        cursor.current = page.next_cursor
        snapshotRevision.current = page.revision
        setDevices(currentDevices => {
          const present = new Map(currentDevices.map(device => [device.id, device]))
          for (const endpoint of page.devices) {
            if (!endpoint.id) continue
            const model = matterDeviceModel(endpoint, states.current.get(endpoint.id), matterReady.current)
            if (model) present.set(model.id, model)
          }
          return [...present.values()]
        })
      }
      setRevision(value => value + 1)
    } catch (error) {
      if (!active.current) return
      matterReady.current = false
      setControllerAvailable(false)
      setDevices(current => current.map(device => ({ ...device, controller_available: false })))
      if (errorMessage(error) === 'stale_revision') {
        cursor.current = 0
        snapshotRevision.current = undefined
        building.current.clear()
      }
      // Matter may be disabled or restarting; retain the last verified catalog.
      nextPoll.current = Date.now() + 10000
    } finally {
      pending.current = false
    }
  }, [])

  useEffect(() => {
    active.current = true
    void poll()
    const timer = window.setInterval(() => void poll(), 1000)
    return () => { active.current = false; window.clearInterval(timer) }
  }, [poll])

  const execute = useCallback(async (device: Device, url: string, body: string): Promise<void> => {
    if (device.source !== 'matter' || !device.matter_node || device.matter_endpoint === undefined ||
        !device.online || device.controller_available === false) {
      throw new Error('Matter device is unavailable')
    }
    const fields = new URLSearchParams(body)
    const signal = new AbortController().signal
    if (url.endsWith('/light') || url.endsWith('/actor')) {
      const state = fields.get('state')
      const brightness = fields.get('brightness')
      if (state !== null) {
        if (!['on', 'off', 'toggle'].includes(state)) throw new Error('invalid switch command')
        const power = await matterOnOff(device.matter_node, device.matter_endpoint, state as 'on' | 'off' | 'toggle', signal)
        storeState(device.id, { power })
        return
      }
      if (brightness !== null) {
        const percent = Number(brightness)
        if (!Number.isInteger(percent) || percent < 0 || percent > 100 || !device.matter_servers?.includes(8)) throw new Error('invalid light command')
        const level = await matterLevel(device.matter_node, device.matter_endpoint,
          { kind: 'move', level: Math.round(percent * 254 / 100), transitionDeciseconds: 0 }, signal)
        storeState(device.id, { level })
        return
      }
      throw new Error('invalid light command')
    }
    if (url.endsWith('/refresh')) {
      const update: MatterDeviceState = { measurements: { ...(states.current.get(device.id)?.measurements ?? {}) } }
      for (const cluster of readClusters) {
        if (!device.matter_servers?.includes(cluster)) continue
        if (cluster === 6) update.power = await matterOnOff(device.matter_node, device.matter_endpoint, 'read', signal)
        else if (cluster === 8) update.level = await matterLevel(device.matter_node, device.matter_endpoint, { kind: 'read' }, signal)
        else {
          const reading = await matterSensor(device.matter_node, device.matter_endpoint, cluster, signal)
          update.measurements![String(cluster)] = reading
        }
      }
      storeState(device.id, update)
      return
    }
    throw new Error('unsupported Matter device action')
  }, [storeState])

  return { devices, execute, controllerAvailable, revision }
}

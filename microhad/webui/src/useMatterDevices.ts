import { useCallback } from 'react'
import type { Device } from './api'
import { matterContact, matterLevel, matterOnOff, matterSensor } from './matter'

const readClusters = [6, 8, 69, 1026, 1029, 1037, 1066, 91]

// Inventory and confirmed values come exclusively from ordinary /api/status.
export function useMatterDevices() {
  const execute = useCallback(async (device: Device, url: string, body: string): Promise<void> => {
    const node = device.matter_node
    if (!node || !device.online) throw new Error('Matter device is unavailable')
    const endpoints = device.endpoint_descriptors?.length ? device.endpoint_descriptors :
      [{ endpoint: device.matter_endpoint ?? -1, input_clusters: device.matter_servers ?? [], output_clusters: [] }]
    const target = (cluster: number) => {
      const endpoint = endpoints.find(endpoint => endpoint.input_clusters.includes(cluster))?.endpoint
      if (endpoint === undefined || endpoint < 0) throw new Error('Unsupported Matter endpoint')
      return endpoint
    }
    const fields = new URLSearchParams(body)
    const signal = new AbortController().signal
    if (url.endsWith('/light') || url.endsWith('/actor')) {
      const state = fields.get('state')
      const brightness = fields.get('brightness')
      if (state !== null) {
        if (!['on', 'off', 'toggle'].includes(state)) throw new Error('invalid switch command')
        await matterOnOff(node, target(6), state as 'on' | 'off' | 'toggle', signal)
        return
      }
      if (brightness !== null) {
        const percent = Number(brightness)
        if (!Number.isInteger(percent) || percent < 0 || percent > 100) throw new Error('invalid light command')
        await matterLevel(node, target(8),
          { kind: 'move', level: Math.round(percent * 254 / 100), transitionDeciseconds: 0 }, signal)
        return
      }
      throw new Error('invalid light command')
    }
    if (url.endsWith('/refresh')) {
      for (const endpoint of endpoints) for (const cluster of readClusters) {
        if (!endpoint.input_clusters.includes(cluster)) continue
        if (cluster === 6) await matterOnOff(node, endpoint.endpoint, 'read', signal)
        else if (cluster === 69) await matterContact(node, endpoint.endpoint, signal)
        else if (cluster === 8) await matterLevel(node, endpoint.endpoint, { kind: 'read' }, signal)
        else await matterSensor(node, endpoint.endpoint, cluster, signal)
      }
      return
    }
    throw new Error('unsupported Matter device action')
  }, [])
  return { execute }
}

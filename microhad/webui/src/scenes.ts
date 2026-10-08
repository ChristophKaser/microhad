import type { Device } from './api'

export interface SceneState {
  ieee: string
  on: boolean
  brightness: number
}
export interface Scene {
  id: number
  name: string
  states: SceneState[]
}
export interface Group {
  id: number
  name: string
  members: string[]
  scenes: Scene[]
}
export interface Binding {
  ieee: string
  action: string
  groupId: number
  off: boolean
  sceneIds: number[]
}
export interface NativeBinding {
  ieee: string
  groupId: number
}
export interface SceneConfig {
  groups: Group[]
  bindings: Binding[]
  nativeBindings: NativeBinding[]
}

export function parseScenes(text: string): SceneConfig {
  const config: SceneConfig = { groups: [], bindings: [], nativeBindings: [] }
  const groups = new Map<number, Group>()
  const scenes = new Map<string, Scene>()
  for (const [index, line] of text.split(/\r?\n/).entries()) {
    if (!line || line.startsWith('#')) continue
    const fields = line.split('\t')
    const invalid = () => new Error(`Line ${index + 1}: unsupported or incomplete record`)
    const groupId = Number(fields[1])
    const group = groups.get(groupId)
    switch (fields[0]) {
      case 'G': {
        if (
          fields.length !== 3 || !Number.isInteger(groupId) ||
          groupId < 1 || groupId > 0xfff7 || group
        ) throw invalid()
        const newGroup: Group = { id: groupId, name: fields[2], members: [], scenes: [] }
        groups.set(groupId, newGroup)
        config.groups.push(newGroup)
        break
      }
      case 'M':
        if (fields.length !== 3) throw invalid()
        if (!group) throw invalid()
        group.members.push(fields[2])
        break
      case 'S': {
        if (fields.length !== 4) throw invalid()
        if (!group) throw invalid()
        const id = Number(fields[2])
        const key = `${groupId}/${id}`
        if (!Number.isInteger(id) || id < 1 || id > 255 || scenes.has(key)) throw invalid()
        const scene: Scene = { id, name: fields[3], states: [] }
        scenes.set(key, scene)
        group.scenes.push(scene)
        break
      }
      case 'T': {
        const scene = scenes.get(`${groupId}/${fields[2]}`)
        if (fields.length !== 6) throw invalid()
        if (!scene) throw invalid()
        scene.states.push({ ieee: fields[3], on: fields[4] === '1', brightness: Number(fields[5]) })
        break
      }
      case 'B':
        if (fields.length !== 6) throw invalid()
        config.bindings.push({
          ieee: fields[1],
          action: fields[2],
          groupId: Number(fields[3]),
          off: fields[4] === '1',
          sceneIds: fields[5] ? fields[5].split(',').map(Number) : [],
        })
        break
      case 'N':
        if (fields.length !== 3) throw invalid()
        config.nativeBindings.push({ ieee: fields[1], groupId: Number(fields[2]) })
        break
      default:
        throw invalid()
    }
  }
  return config
}

export function nextId(items: { id: number }[], limit: number): number {
  for (let id = 1; id <= limit; id++) if (!items.some((item) => item.id === id)) return id
  return 0
}

export function isDimmable(device: Device): boolean {
  return (
    device.has_brightness ||
    device.endpoint_descriptors.some((endpoint) => endpoint.input_clusters.includes(8))
  )
}

export function newState(address: string, devices: Device[]): SceneState {
  const device = devices.find((item) => item.ieee_address === address)
  const brightness =
    device && isDimmable(device) ? (device.has_brightness ? device.brightness_percent : 100) : -1
  return { ieee: address, on: device?.light_on ?? true, brightness }
}

export function serializeScenes(config: SceneConfig, devices: Device[]): string {
  const lines = ['# microhad-zigbee-scenes-v2']
  const name = (value: string) => {
    if (!value.trim() || /[\t\r\n]/.test(value))
      throw new Error('Names must be nonempty and cannot contain tabs or line breaks')
    return value
  }
  for (const group of config.groups) {
    if (!group.members.length) throw new Error('Every group needs at least one light')
    lines.push(`G\t${group.id}\t${name(group.name)}`)
    group.members.forEach((member) => lines.push(`M\t${group.id}\t${member}`))
    for (const scene of group.scenes) {
      lines.push(`S\t${group.id}\t${scene.id}\t${name(scene.name)}`)
      for (const member of group.members) {
        const state = scene.states.find((item) => item.ieee === member) ?? newState(member, devices)
        if (!Number.isInteger(state.brightness) || state.brightness < -1 || state.brightness > 100)
          throw new Error('Brightness must be from 0 to 100')
        lines.push(
          `T\t${group.id}\t${scene.id}\t${member}\t${state.on ? 1 : 0}\t${state.brightness}`
        )
      }
    }
  }
  for (const binding of config.bindings) {
    if (!binding.ieee || !binding.groupId || (!binding.off && !binding.sceneIds.length))
      throw new Error('Each binding needs a remote, group, and scene')
    lines.push(
      `B\t${binding.ieee}\t${binding.action}\t${binding.groupId}\t${binding.off ? 1 : 0}\t${
        binding.off ? '' : binding.sceneIds.join(',')
      }`
    )
  }
  const nativeRemotes = new Set<string>()
  for (const binding of config.nativeBindings) {
    if (!binding.ieee || nativeRemotes.has(binding.ieee) || binding.groupId === 901 ||
        !config.groups.some((group) => group.id === binding.groupId))
      throw new Error('Each native remote needs one valid light group')
    nativeRemotes.add(binding.ieee)
    lines.push(`N\t${binding.ieee}\t${binding.groupId}`)
  }
  return `${lines.join('\n')}\n`
}

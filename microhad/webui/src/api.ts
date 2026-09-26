export interface EndpointDescriptor {
  endpoint: number
  profile_id: number
  device_id: number
  device_version: number
  input_clusters: number[]
  output_clusters: number[]
}

export interface Device {
  ieee_address: string
  name: string
  kind: string
  has_contact: boolean
  contact_open: boolean
  has_temperature: boolean
  temperature_celsius: number
  has_humidity: boolean
  humidity_percent: number
  has_occupancy: boolean
  occupied: boolean
  has_battery: boolean
  battery_percent: number
  link_quality: number
  has_light: boolean
  light_on: boolean
  has_brightness: boolean
  brightness_percent: number
  has_remote: boolean
  has_low_temperature_alert: boolean
  low_temperature_alert: number
  has_high_temperature_alert: boolean
  high_temperature_alert: number
  contact_initial_alert_minutes: number
  contact_reminder_alert_minutes: number
  sensor_refresh_interval_minutes: number
  last_seen: number
  manufacturer_name: string
  model_id: string
  endpoint_descriptors: EndpointDescriptor[]
}

export interface SceneSummary {
  id: number
  name: string
}
export interface SceneGroupSummary {
  id: number
  name: string
  members: string[]
  scenes: SceneSummary[]
}
export interface Status {
  online: boolean
  network_forming: boolean
  network_formed: boolean
  serial_rx_bytes: number
  ash_rx_frames: number
  joining_until: number
  joining_preparing: boolean
  notifications_enabled: boolean
  devices: Device[]
  scene_groups: SceneGroupSummary[]
  native_bindings: { ieee_address: string; group_id: number; status: string }[]
}

export type HistorySample = [sequence: number, timestamp: number, metric: 1 | 2 | 3, value: number]
export interface HistoryPage {
  samples: HistorySample[]
  next_before: number
}

export async function jsonRequest<T>(url: string, options?: RequestInit): Promise<T> {
  const response = await fetch(url, options)
  const data = (await response.json()) as T & { ok?: boolean; error?: string }
  if (!response.ok || data.ok === false)
    throw new Error(data.error || `Request failed (${response.status})`)
  return data
}

export async function textRequest(url: string): Promise<string> {
  const response = await fetch(url)
  if (!response.ok) throw new Error(`Request failed (${response.status})`)
  return response.text()
}

export function post<T = unknown>(url: string, body = ''): Promise<T> {
  return jsonRequest<T>(url, { method: 'POST', body })
}

export function form(fields: Record<string, string | number>): string {
  return new URLSearchParams(
    Object.entries(fields).map(([key, value]) => [key, String(value)])
  ).toString()
}

export function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : String(error)
}

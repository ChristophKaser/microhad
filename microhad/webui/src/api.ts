export interface EndpointDescriptor {
  endpoint: number
  profile_id: number
  device_id: number
  device_version: number
  input_clusters: number[]
  output_clusters: number[]
}

export interface NodeDescriptor {
  raw_hex: string
  logical_device_type: number
  complex_descriptor_available: boolean
  user_descriptor_available: boolean
  aps_flags: number
  frequency_band: number
  mac_capability_flags: number
  alternate_pan_coordinator: boolean
  full_function_device: boolean
  mains_powered: boolean
  receiver_on_when_idle: boolean
  security_capability: boolean
  allocate_address: boolean
  manufacturer_code: number
  max_buffer_size: number
  max_incoming_transfer_size: number
  server_mask: number
  max_outgoing_transfer_size: number
  descriptor_capability: number
}

export interface Device {
  id: string
  source: 'zigbee' | 'shelly' | 'fritz_box'
  component: string
  external_id: string
  ieee_address: string
  name: string
  kind: string
  has_contact: boolean
  contact_known: boolean
  contact_open: boolean
  has_temperature: boolean
  temperature_known: boolean
  temperature_celsius: number
  has_humidity: boolean
  humidity_percent: number
  has_occupancy: boolean
  occupied: boolean
  has_battery: boolean
  battery_percent: number
  link_quality: number
  has_wifi_signal: boolean
  wifi_signal_percent: number
  has_light: boolean
  light_on: boolean
  has_brightness: boolean
  brightness_percent: number
  has_remote: boolean
  has_actor: boolean
  actor_known: boolean
  actor_on: boolean
  has_power: boolean
  power_known: boolean
  power_watts: number
  has_low_temperature_alert: boolean
  low_temperature_alert: number
  has_high_temperature_alert: boolean
  high_temperature_alert: number
  contact_initial_alert_minutes: number
  contact_reminder_alert_minutes: number
  contact_trigger_mode: boolean
  contact_trigger_cooldown_seconds: number
  contact_trigger_active: boolean
  sensor_refresh_interval_minutes: number
  value_alerts: Record<string, { has_low: boolean; low: number; has_high: boolean; high: number; hysteresis: number }>
  last_seen: number
  manufacturer_name: string
  model_id: string
  node_descriptor: NodeDescriptor | null
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
  device_configurations: DeviceConfiguration[]
}

export interface ControllerStats {
  device_uptime_seconds: number | null
  service_uptime_seconds: number | null
  serial_opened_at: number
  serial_uptime_seconds: number | null
  cpu_usage_percent: number | null
  cpu_load_1: number | null
  cpu_load_5: number | null
  cpu_load_15: number | null
  memory_total_bytes: number | null
  memory_free_bytes: number | null
  process_memory_bytes: number | null
  log_file_bytes: number
  values_file_bytes: number
  storage_total_bytes: number | null
  storage_free_bytes: number | null
  storage_used_bytes: number | null
  threads: { tid: number; name: string; cpu_usage_percent: number | null }[]
}

export interface ExternalDevice {
  id: string
  name: string
  host: string
  callback_host: string
  model: 'uni' | 'plug' | 'plugs'
  http_auth?: boolean
  input_index: number
  temperature_index: number
  relay_index: number
  input_indices?: string
  temperature_indices?: string
  relay_indices?: string
  poll_seconds: number
  invert_contact: boolean
  callback_mode: boolean
  callback_urls?: Record<string, string>
}

export interface DeviceConfiguration {
  ieee_address: string
  node_descriptor_status: string
  groups_status: string
  groups: number[]
  bindings_status: string
  bindings: {
    cluster_id: number
    group_destination: boolean
    group_id?: number
    ieee_address?: string
    endpoint?: number
  }[]
}

export type HistorySample = [sequence: number, timestamp: number, metric: 1 | 2 | 3 | 4 | 5 | 6, value: number]
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

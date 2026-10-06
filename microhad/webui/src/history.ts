import type { Device, HistoryMetric, HistorySample } from './api'

const names: Record<HistoryMetric, string> = {
  1: 'Temperature', 2: 'Humidity', 3: 'Contact', 4: 'Power', 5: 'Occupancy', 6: 'Battery',
  7: 'CO₂', 8: 'PM2.5', 9: 'Air quality', 10: 'Light', 11: 'Brightness',
}
const defaultUnits: Record<HistoryMetric, string> = {
  1: '°C', 2: '%', 3: '', 4: 'W', 5: '', 6: '%', 7: 'ppm', 8: 'ug/m3', 9: '', 10: '', 11: '%',
}
const quality = ['good', 'fair', 'moderate', 'poor', 'very_poor', 'extremely_poor']

export const historyDeviceId = (device: Device): string => device.source === 'shelly'
  ? `shelly:${device.external_id}:${device.component}`
  : device.source === 'matter' ? device.external_id
    : device.source === 'fritz_box' ? `fritz:${device.external_id}:${device.component}`
      : `zigbee:${device.ieee_address}`

export const historyName = (metric: HistoryMetric): string => names[metric]
export const historyDiscrete = (metric: HistoryMetric): boolean => [3, 5, 9, 10].includes(metric)
export const historyUnit = (metric: HistoryMetric, sample?: HistorySample): string => {
  const unit = sample?.[4]
  if (!unit) return defaultUnits[metric]
  if (unit === 'celsius') return '°C'
  if (unit === 'percent') return '%'
  if (unit === 'watts') return 'W'
  return unit
}
export const historyY = (metric: HistoryMetric, sample: HistorySample): number => {
  if (metric === 9) return Math.max(0, quality.indexOf(String(sample[3])))
  const value = Number(sample[3])
  if (sample[4] !== undefined) return value
  if (metric === 1 || metric === 2) return value / 100
  if (metric === 4) return value / 10
  return value
}
export const historyLabel = (metric: HistoryMetric, sample: HistorySample): string => {
  if (metric === 3) return Number(sample[3]) ? 'Open' : 'Closed'
  if (metric === 5) return Number(sample[3]) ? 'Occupied' : 'Clear'
  if (metric === 9) return String(sample[3]).replace('_', ' ')
  if (metric === 10) return Number(sample[3]) ? 'On' : 'Off'
  const precision = metric === 4 ? 1 : metric === 6 || metric === 11 ? 0 : 2
  return `${historyY(metric, sample).toFixed(precision)}${historyUnit(metric, sample) ? ` ${historyUnit(metric, sample)}` : ''}`
}
export const historyTick = (metric: HistoryMetric, value: number): string => {
  if (metric === 3) return value ? 'Open' : 'Closed'
  if (metric === 5) return value ? 'Occupied' : 'Clear'
  if (metric === 9) return quality[Math.round(value)]?.replace('_', ' ') ?? ''
  if (metric === 10) return value ? 'On' : 'Off'
  return String(value)
}

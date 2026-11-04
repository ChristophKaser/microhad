import type { Device } from './api'

export type Catalog = { id: string; label: string; url: string; manufacturer?: number; gateway_only?: boolean }
export type Firmware = { source: string; url: string; version: number; size?: number; manufacturer: number; imageType: number; model?: string; names?: string[]; minHardware?: number; maxHardware?: number; minVersion?: number; maxVersion?: number }
type Row = Record<string, unknown>
const uint = (v: unknown, max = 0xffffffff): v is number => typeof v === 'number' && Number.isInteger(v) && v >= 0 && v <= max
export function firmwareUrl(value: unknown): string | undefined {
  if (typeof value !== 'string') return
  try {
    const url = new URL(value)
    if (['https:', 'http:'].includes(url.protocol) && !url.username && !url.password) return url.href
  } catch { /* Ignore invalid catalog entries. */ }
}
export function parseCatalog(catalog: Catalog, data: unknown): Firmware[] {
  if (!Array.isArray(data)) throw new Error('Invalid firmware catalog')
  return data.flatMap((entry: unknown): Firmware[] => {
    if (!entry || typeof entry !== 'object') return []
    const row = entry as Row
    const vendor = catalog.id === 'ikea' || catalog.id === 'sonoff'
    let version = vendor ? row.fw_file_version : row.fileVersion
    if (catalog.id === 'ikea') {
      if (typeof row.fw_sha3_256 === 'string') {
        const match = String(row.fw_binary_url).match(/_v(\d+)_/)
        version = match ? Number(match[1]) : undefined
      } else {
        if (row.fw_type !== 2 || !uint(row.fw_file_version_MSB, 65535) || !uint(row.fw_file_version_LSB, 65535)) return []
        version = row.fw_file_version_MSB * 65536 + row.fw_file_version_LSB
      }
    }
    const manufacturer = vendor ? (row.fw_manufacturer_id ?? catalog.manufacturer) : row.manufacturerCode
    const imageType = vendor ? row.fw_image_type : row.imageType
    const url = firmwareUrl(vendor ? row.fw_binary_url : row.url)
    if (!url || !uint(version) || !uint(manufacturer, 65535) || !uint(imageType, 65535)) return []
    const model = vendor ? row.model_id : row.modelId
    const names = row.manufacturerName
    if (model !== undefined && typeof model !== 'string') return []
    if (names !== undefined && (!Array.isArray(names) || !names.every(name => typeof name === 'string'))) return []
    const limits = ['hardwareVersionMin', 'hardwareVersionMax', 'minFileVersion', 'maxFileVersion'] as const
    if (limits.some(key => row[key] !== undefined && !uint(row[key]))) return []
    // These IKEA image types are excluded by the upstream provider due to unsafe releases.
    if (catalog.id === 'ikea' && [8704, 8710].includes(imageType)) return []
    return [{ source: catalog.label, url, version, manufacturer, imageType, model: model as string | undefined,
      names: names as string[] | undefined, size: uint(vendor ? row.fw_filesize : row.fileSize) ? Number(vendor ? row.fw_filesize : row.fileSize) : undefined,
      minHardware: row.hardwareVersionMin as number | undefined, maxHardware: row.hardwareVersionMax as number | undefined,
      minVersion: row.minFileVersion as number | undefined, maxVersion: row.maxFileVersion as number | undefined }]
  })
}
export function matchingFirmware(device: Device, images: Firmware[]): Firmware[] {
  const results = images.filter(image => {
    if (device.source !== 'zigbee') return false
    if (image.model !== undefined && image.model !== device.model_id) return false
    if (image.names !== undefined && !image.names.includes(device.manufacturer_name)) return false
    const manufacturer = device.ota_manufacturer ?? device.node_descriptor?.manufacturer_code
    if (manufacturer == null || image.manufacturer !== manufacturer) return false
    if (device.ota_image_type == null || image.imageType !== device.ota_image_type) return false
    if (image.minHardware !== undefined && (device.hardware_version == null || device.hardware_version < image.minHardware)) return false
    if (image.maxHardware !== undefined && (device.hardware_version == null || device.hardware_version > image.maxHardware)) return false
    if (image.minVersion !== undefined && (device.ota_file_version == null || device.ota_file_version < image.minVersion)) return false
    if (image.maxVersion !== undefined && (device.ota_file_version == null || device.ota_file_version > image.maxVersion)) return false
    return true
  })
  // Manufacturer feeds are loaded before the community index.
  return results.filter((image, i) => results.findIndex(other => other.manufacturer === image.manufacturer && other.imageType === image.imageType && other.version === image.version) === i)
    .sort((a, b) => b.version - a.version)
}

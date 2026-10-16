import { MatterError, type MatterSession } from './matter'

const SERVICE = '0000fff6-0000-1000-8000-00805f9b34fb'
const C1 = '18ee2ef5-263d-4559-959f-4f9c429f9d11'
const C2 = '18ee2ef5-263d-4559-959f-4f9c429f9d12'
const forbiddenPins = new Set([11111111, 22222222, 33333333, 44444444, 55555555, 66666666, 77777777, 88888888, 12345678, 87654321])

type BluetoothCharacteristic = { startNotifications(): Promise<unknown>; addEventListener(type: 'characteristicvaluechanged', listener: (event: Event) => void): void; writeValueWithResponse?: (value: BufferSource) => Promise<void>; writeValue(value: BufferSource): Promise<void> }
type BluetoothServer = { getPrimaryService(service: string): Promise<{ getCharacteristic(id: string): Promise<BluetoothCharacteristic> }> }
type BluetoothDevice = { gatt?: { connect(): Promise<BluetoothServer>; disconnect(): void }; addEventListener(type: 'gattserverdisconnected', listener: () => void): void }
declare global { interface Navigator { bluetooth?: { requestDevice(options: { filters: Array<{ services: string[] }> }): Promise<BluetoothDevice> } } }

export type SetupCode = { discriminator: number; pin: number; ble: boolean }
export function webBluetoothAvailable(): boolean { return window.isSecureContext && !!navigator.bluetooth }
export async function chooseMatterDevice(): Promise<BluetoothDevice> {
  if (!navigator.bluetooth) throw new MatterError('unsupported')
  return navigator.bluetooth.requestDevice({ filters: [{ services: [SERVICE] }] })
}

function pin(value: number): boolean { return value >= 1 && value <= 99999998 && !forbiddenPins.has(value) }
function multiply(left: number, right: number): number {
  if (left < 5 && right < 5) return (left + right) % 5
  if (left < 5) return (left + right - 5) % 5 + 5
  if (right < 5) return (left - 5 - right + 5) % 5 + 5
  return (left - right + 5) % 5
}
function validManual(value: string): boolean {
  const permutation = [1, 5, 7, 6, 2, 8, 3, 0, 9, 4]; let result = 0
  for (let index = 0; index < value.length; index++) { let digit = Number(value[value.length - index - 1]); for (let i = 0; i < index % 8; i++) digit = permutation[digit]; result = multiply(result, digit) }
  return result === 0
}
function base38(value: string): Uint8Array {
  const alphabet = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-.'; if (!value || ![0, 2, 4].includes(value.length % 5)) throw new MatterError('malformed')
  const out: number[] = []
  for (let at = 0; at < value.length; ) { const count = Math.min(5, value.length - at); const width = count === 5 ? 3 : count === 4 ? 2 : count === 2 ? 1 : 0; if (!width) throw new MatterError('malformed'); let number = 0; let scale = 1; for (let i = 0; i < count; i++) { const digit = alphabet.indexOf(value[at + i]); if (digit < 0) throw new MatterError('malformed'); number += digit * scale; scale *= 38 } if (number >= 2 ** (width * 8)) throw new MatterError('malformed'); for (let i = 0; i < width; i++) out.push((number >>> (8 * i)) & 255); at += count }
  return new Uint8Array(out)
}
function bits(bytes: Uint8Array, at: number, count: number): number { let value = 0; for (let i = 0; i < count; i++) value |= ((bytes[Math.floor((at + i) / 8)] >>> ((at + i) % 8)) & 1) << i; return value }
export function setupCode(input: string): SetupCode {
  let value = input.trim(); const ikea = /^https:\/\/matter\.ikea\.com\/v1\/add\/[^/?#]+\?r=([^,&]+),((?:MT:).+)$/i.exec(value)
  if (ikea) value = ikea[2]
  if (value.startsWith('MT:')) { const bytes = base38(value.slice(3)); if (bytes.length < 11 || bits(bytes, 0, 3) !== 0) throw new MatterError('malformed'); const flow = bits(bytes, 35, 2); const passcode = bits(bytes, 57, 27); if (flow === 2 || !pin(passcode)) throw new MatterError('unsupported'); return { discriminator: bits(bytes, 45, 12), pin: passcode, ble: (bits(bytes, 37, 8) & 2) !== 0 } }
  value = value.replace(/[ -]/g, ''); if (!/^\d{11}$/.test(value) && !/^\d{21}$/.test(value) || !validManual(value)) throw new MatterError('malformed')
  const first = Number(value[0]); const second = Number(value.slice(1, 6)); const third = Number(value.slice(6, 10)); const long = (first & 4) !== 0; const passcode = (third << 14) | (second & 16383)
  if (first > 7 || second > 65535 || third > 8191 || long !== (value.length === 21) || long || !pin(passcode)) throw new MatterError('unsupported')
  return { discriminator: ((first & 3) << 2) | (second >>> 14), pin: passcode, ble: true }
}
function hex(bytes: Uint8Array): string { return [...bytes].map(value => value.toString(16).padStart(2, '0')).join('') }
function bytes(value: string): Uint8Array { if (!/^(?:[0-9a-f]{2})+$/.test(value)) throw new MatterError('protocol'); return new Uint8Array(value.match(/../g)!.map(item => Number.parseInt(item, 16))) }
function randomAddress(): string { const value = new Uint8Array(6); crypto.getRandomValues(value); value[0] = (value[0] | 2) & 254; return hex(value) }

export class BrowserMatterRelay {
  private socket: WebSocket | null = null; private next = 1; private generation = 0; private write = Promise.resolve(); private c1: BluetoothCharacteristic | null = null
  constructor(private readonly session: string, private readonly progress: (value: MatterSession) => void, private readonly fail: (error: MatterError) => void) {}
  async connect(device: BluetoothDevice): Promise<void> {
    const server = await device.gatt!.connect(); const service = await server.getPrimaryService(SERVICE)
    this.c1 = await service.getCharacteristic(C1); const c2 = await service.getCharacteristic(C2); await c2.startNotifications(); c2.addEventListener('characteristicvaluechanged', event => { const value = (event.target as { value?: DataView }).value; if (value) void this.call(7, { generation: this.generation, sequence: this.next, data: hex(new Uint8Array(value.buffer, value.byteOffset, value.byteLength)) }).then(() => { this.next++ }).catch(() => this.fail(new MatterError('transport'))) })
    device.addEventListener('gattserverdisconnected', () => this.close())
    await this.open(); const attached = await this.call(6, { address: randomAddress(), mtu: 23 }) as { generation: number }; if (!Number.isInteger(attached.generation) || attached.generation < 1) throw new MatterError('protocol'); this.generation = attached.generation
  }
  private open(): Promise<void> { return new Promise((resolve, reject) => { const protocol = location.protocol === 'https:' ? 'wss:' : 'ws:'; const socket = this.socket = new WebSocket(`${protocol}//${location.host}/api/matter/relay`); socket.onopen = () => { void this.call(4, {}).then(() => resolve(), reject) }; socket.onerror = () => reject(new MatterError('transport')); socket.onmessage = event => this.message(event.data) }) }
  private message(raw: unknown) { try { const data = JSON.parse(String(raw)) as Record<string, unknown>; if (data.event === 'commissioning') { this.progress(data.data as MatterSession); return } if (data.event !== 'ble_write' || !data.data || typeof data.data !== 'object') throw new MatterError('protocol'); const write = data.data as Record<string, unknown>; if (write.generation !== this.generation || !Number.isInteger(write.write) || typeof write.data !== 'string' || !this.c1) throw new MatterError('protocol'); this.write = this.write.then(async () => { const payload = bytes(write.data as string); if (this.c1!.writeValueWithResponse) await this.c1!.writeValueWithResponse(payload); else await this.c1!.writeValue(payload); await this.call(8, { generation: this.generation, write: write.write, success: true }) }).catch(() => this.fail(new MatterError('transport'))) } catch (error) { this.fail(error instanceof MatterError ? error : new MatterError('protocol')) } }
  private call(opcode: number, body: Record<string, unknown>): Promise<unknown> { return new Promise((resolve, reject) => { const id = this.next++; const socket = this.socket; if (!socket || socket.readyState !== WebSocket.OPEN) { reject(new MatterError('transport')); return } const timer = window.setTimeout(() => reject(new MatterError('timeout')), 10000); const previous = socket.onmessage; socket.onmessage = event => { this.message(event.data); try { const data = JSON.parse(String(event.data)) as Record<string, unknown>; if (data.id === id && data.response && typeof data.response === 'object') { window.clearTimeout(timer); socket.onmessage = previous; const response = data.response as Record<string, unknown>; if (response.ok !== true) reject(new MatterError(String(response.error))); else resolve(response.result) } } catch { } }; socket.send(JSON.stringify({ schema: 1, id, opcode, body: { schema: 1, session: this.session, ...body } })) }) }
  close() { this.socket?.close(); this.socket = null; this.c1 = null }
}

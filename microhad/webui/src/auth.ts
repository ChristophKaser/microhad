import { sha256 } from '@noble/hashes/sha256'
import { hmac } from '@noble/hashes/hmac'
import { pbkdf2Async } from '@noble/hashes/pbkdf2'
import { bytesToHex, hexToBytes, utf8ToBytes } from '@noble/hashes/utils'

export const roles = ['guest', 'user', 'administrator'] as const
const key = 'microhad-session'
export function sessionToken(): string { return sessionStorage.getItem(key) || localStorage.getItem(key) || '' }
export function clearSession() { sessionStorage.removeItem(key); localStorage.removeItem(key) }
export async function authFetch(url: string, options?: RequestInit): Promise<Response> {
  const headers = new Headers(options?.headers)
  const token = sessionToken()
  if (token) headers.set('Authorization', `Bearer ${token}`)
  const response = await fetch(url, { ...options, headers, redirect: 'error' })
  if (response.status === 401 || (response.status === 403 && options?.method && options.method !== 'GET')) window.dispatchEvent(new Event('microhad-login-required'))
  return response
}
async function request(path: string, body?: unknown) {
  const response = await authFetch(`/api/auth/${path}`, body === undefined ? undefined : { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) })
  const value = await response.json()
  if (!response.ok) throw new Error(value.error || 'Authentication failed')
  return value
}
export async function accessStatus(): Promise<{ role: number; passwords: boolean[] }> { return request('status') }
export async function login(role: string, password: string, remember: boolean) {
  const challenge = await request('challenge', { role })
  if (challenge.open) { clearSession(); return }
  if (challenge.iterations !== 100000 || !/^[0-9a-f]{32}$/.test(challenge.salt) || !/^[0-9a-f]{64}$/.test(challenge.challenge)) throw new Error('Invalid login challenge')
  const verifier = await pbkdf2Async(sha256, utf8ToBytes(password), hexToBytes(challenge.salt), { c: 100000, dkLen: 32 })
  const mode = remember ? 'persistent' : 'browser'
  const proof = bytesToHex(hmac(sha256, verifier, utf8ToBytes(`microhad-login-v1\n${challenge.challenge}\n${role}\n${mode}`)))
  verifier.fill(0)
  const result = await request('login', { challenge: challenge.challenge, proof, mode })
  clearSession()
  ;(remember ? localStorage : sessionStorage).setItem(key, result.token)
}
export async function logout() { clearSession() }
export async function setPassword(role: string, password: string) {
  let salt = '', verifier = ''
  if (password) {
    salt = bytesToHex(crypto.getRandomValues(new Uint8Array(16)))
    const derived = await pbkdf2Async(sha256, utf8ToBytes(password), hexToBytes(salt), { c: 100000, dkLen: 32 })
    verifier = bytesToHex(derived); derived.fill(0)
  }
  await request('password', { role, salt, verifier }); clearSession()
}

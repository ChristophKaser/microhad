const { test } = require('node:test')
const assert = require('node:assert/strict')
const { pbkdf2Sync, createHmac, webcrypto } = require('node:crypto')
const vm = require('node:vm')
const esbuild = require('esbuild')
const path = require('node:path')

function storage() {
  const values = new Map()
  return { getItem: key => values.get(key) || null, setItem: (key, value) => values.set(key, value), removeItem: key => values.delete(key) }
}
const compiled = esbuild.buildSync({ entryPoints: [path.resolve(__dirname, '../src/auth.ts')], bundle: true, write: false, format: 'iife', globalName: 'auth', platform: 'browser' }).outputFiles[0].text
function setup(fetch) {
  const context = vm.createContext({ fetch, sessionStorage: storage(), localStorage: storage(), Headers, TextEncoder, Uint8Array, crypto: webcrypto, setTimeout, window: { dispatchEvent() {} }, Event })
  vm.runInContext(compiled, context)
  return context
}

test('HTTP login sends a single-use proof and stores sessions according to remember me', async () => {
  const salt = '000102030405060708090a0b0c0d0e0f', challenge = 'a'.repeat(64)
  for (const remember of [false, true]) {
    const calls = []
    const context = setup(async (url, options) => {
      calls.push({ url, options })
      const value = url.endsWith('/challenge') ? { salt, challenge, iterations: 100000 } : { token: 'b'.repeat(64) }
      return { ok: true, status: 200, json: async () => value }
    })
    await context.auth.login('administrator', 'pássword', remember)
    const body = JSON.parse(calls[1].options.body)
    const key = pbkdf2Sync('pássword', Buffer.from(salt, 'hex'), 100000, 32, 'sha256')
    assert.equal(body.proof, createHmac('sha256', key).update(`microhad-login-v1\n${challenge}\nadministrator\n${remember ? 'persistent' : 'browser'}`).digest('hex'))
    assert.equal(JSON.stringify(calls).includes('pássword'), false)
    assert.equal((remember ? context.localStorage : context.sessionStorage).getItem('microhad-session'), 'b'.repeat(64))
    assert.equal((remember ? context.sessionStorage : context.localStorage).getItem('microhad-session'), null)
    await context.auth.authFetch('/api/status')
    assert.equal(calls[2].options.headers.get('Authorization'), `Bearer ${'b'.repeat(64)}`)
  }
})

test('password updates send only salted derived verifiers and clear old sessions', async () => {
  let sent
  const context = setup(async (url, options) => { sent = JSON.parse(options.body); return { ok: true, status: 200, json: async () => ({ ok: true }) } })
  context.localStorage.setItem('microhad-session', 'old')
  await context.auth.setPassword('guest', 'new password')
  assert.equal(sent.role, 'guest')
  assert.equal(sent.salt.length, 32)
  assert.equal(sent.verifier, pbkdf2Sync('new password', Buffer.from(sent.salt, 'hex'), 100000, 32, 'sha256').toString('hex'))
  assert.equal(JSON.stringify(sent).includes('new password'), false)
  assert.equal(context.auth.sessionToken(), '')
  await context.auth.setPassword('guest', '')
  assert.equal(sent.salt, '')
  assert.equal(sent.verifier, '')
})


test('logout clears both browser stores without contacting the gateway', async () => {
  let calls = 0
  const context = setup(async () => { calls++; throw new Error('offline') })
  context.localStorage.setItem('microhad-session', 'remembered')
  context.sessionStorage.setItem('microhad-session', 'temporary')
  await context.auth.logout()
  assert.equal(context.auth.sessionToken(), '')
  assert.equal(context.localStorage.getItem('microhad-session'), null)
  assert.equal(context.sessionStorage.getItem('microhad-session'), null)
  assert.equal(calls, 0)
})

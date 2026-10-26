import { readFileSync, mkdirSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs'
import { execFileSync } from 'node:child_process'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { transform } from 'esbuild'
import { fileURLToPath } from 'node:url'

// Ace 1.45.2 embeds luaparse 0.3.1. Patch readable source, never minified text.
export function patchLuaWorker(source) {
  const directory = mkdtempSync(join(tmpdir(), 'microhad-lua-worker-'))
  try {
    const filename = join(directory, 'worker-lua.js')
    writeFileSync(filename, source)
    execFileSync('patch', ['--batch', '--forward', '--silent', '-p1', '-i',
      fileURLToPath(new URL('../patches/ace-lua-5.5.patch', import.meta.url))],
      { cwd: directory, encoding: 'utf8' })
    return readFileSync(filename, 'utf8')
  } finally {
    rmSync(directory, { recursive: true, force: true })
  }
}

export async function generateLuaWorker() {
  const root = new URL('../', import.meta.url)
  const patched = patchLuaWorker(readFileSync(new URL('node_modules/ace-builds/src/worker-lua.js', root), 'utf8'))
  const output = new URL('node_modules/.cache/microhad-worker-lua.js', root)
  mkdirSync(fileURLToPath(new URL('node_modules/.cache/', root)), { recursive: true })
  writeFileSync(output, (await transform(patched + `
const parser = new (require('ace/mode/lua_worker').Worker)({on() {}, emit() {}}).parser;
onmessage = ({data}) => {
  let errors = [];
  try { parser.parse(data.source); }
  catch (error) { errors = [{row: error.line - 1, column: error.column, text: error.message}]; }
  postMessage({id: data.id, errors});
};
`, { minify: true, target: 'es2020' })).code)
}

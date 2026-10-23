import { readFileSync, mkdirSync, writeFileSync } from 'node:fs'
import { createHash } from 'node:crypto'
import { transform } from 'esbuild'
import { fileURLToPath } from 'node:url'

// Ace 1.44.0 embeds luaparse 0.3.1. Patch readable source, never minified text.
export function patchLuaWorker(source) {
  if (createHash('sha256').update(source).digest('hex') !== 'fecabfc2b296172539cdd2e0323732f99336305965f5e356dbd3a71872ea9812') {
    throw new Error('Ace Lua worker changed: review the Lua 5.4 patch before upgrading')
  }
  const replace = (before, after) => {
    if (source.split(before).length !== 2) throw new Error('Lua worker patch context mismatch')
    source = source.replace(before, after)
  }
  replace('index - escStart > 6', 'index - escStart > 8')
  replace('codepoint > 0x10ffff', 'codepoint > 0x7fffffff')
  replace(`  function parseLocalStatement(flowContext) {\n    var name`, `  function parseLocalStatement(flowContext) {\n    var hasClose = false;\n    var name`)
  replace(`        name = parseIdentifier();\n\n        variables.push(name);`, `        name = parseIdentifier();
        if (consume('<')) {
          var attributeToken = token;
          var attribute = parseIdentifier().name;
          if (attribute !== 'const' && attribute !== 'close')
            raise(attributeToken, "unknown attribute '%1'", attribute);
          if (attribute === 'close' && hasClose)
            raise(attributeToken, "multiple to-be-closed variables in local list");
          if (attribute === 'close') hasClose = true;
          expect('>');
          name.attribute = attribute;
        }

        variables.push(name);`)
  return source
}

export async function generateLuaWorker() {
  const root = new URL('../', import.meta.url)
  const patched = patchLuaWorker(readFileSync(new URL('node_modules/ace-builds/src/worker-lua.js', root), 'utf8'))
  const output = new URL('node_modules/.cache/microhad-worker-lua.js', root)
  mkdirSync(fileURLToPath(new URL('node_modules/.cache/', root)), { recursive: true })
  writeFileSync(output, (await transform(patched, { minify: true, target: 'es2020' })).code)
}

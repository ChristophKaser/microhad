import { gzipSync } from 'node:zlib'
import { readdirSync, readFileSync, renameSync, rmSync, writeFileSync } from 'node:fs'
import { join, relative } from 'node:path'
import { fileURLToPath } from 'node:url'

const dist = fileURLToPath(new URL('../dist/', import.meta.url))
const files = []

function collect(directory) {
  for (const entry of readdirSync(directory, { withFileTypes: true })) {
    const filename = join(directory, entry.name)
    if (entry.isDirectory()) collect(filename)
    else if (entry.isFile()) files.push(filename)
  }
}

function format(bytes) {
  return String(bytes)
}

collect(dist)
let rawTotal = 0
let compressedTotal = 0
for (const filename of files.sort()) {
  if (filename.endsWith('.gz')) throw new Error(`unexpected pre-compressed artifact: ${filename}`)
  const source = readFileSync(filename)
  const compressed = gzipSync(source, { level: 9, mtime: 0 })
  const temporary = `${filename}.gz.new`
  writeFileSync(temporary, compressed)
  renameSync(temporary, `${filename}.gz`)
  rmSync(filename)
  rawTotal += source.length
  compressedTotal += compressed.length
  const saved = source.length - compressed.length
  const percent = source.length === 0 ? 0 : (saved * 100 / source.length)
  console.log(`${relative(dist, filename)}\t${format(source.length)}\t${format(compressed.length)}\t${percent.toFixed(1)}%`)
}
const totalSaved = rawTotal - compressedTotal
const totalPercent = rawTotal === 0 ? 0 : (totalSaved * 100 / rawTotal)
console.log(`TOTAL\t${format(rawTotal)}\t${format(compressedTotal)}\t${totalPercent.toFixed(1)}%`)

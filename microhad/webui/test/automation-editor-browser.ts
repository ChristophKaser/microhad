import * as ace from 'ace-builds'
import { attachEditor } from '../src/aceAutomation'

const output = document.getElementById('result')!
const assert = (condition: unknown, message: string) => { if (!condition) throw new Error(message) }
const pause = (ms: number) => new Promise(resolve => setTimeout(resolve, ms))
async function main() {
  const host = document.getElementById('editor') as HTMLDivElement
  let source = '', workerFailed = false
  const catalog = { devices: [{ ieee_address: 'abc', id: 'different', name: 'Desk "lamp"', source: 'zigbee', kind: 'light', has_light: true }], groups: [] }
  const instance = attachEditor(host, '', value => { source = value }, () => catalog as never, () => { workerFailed = true })
  const editor = ace.edit(host)
  instance.setSource('local x <const> = 1\nlocal y <close> = nil')
  await pause(1200)
  assert(!workerFailed, 'worker failed')
  assert(editor.session.getAnnotations().length === 0, 'valid Lua 5.4 syntax flagged')
  instance.setSource('local x <wat> = 1')
  await pause(1200)
  assert(editor.session.getAnnotations().some(item => item.type === 'error'), 'missing live syntax annotation')
  instance.setSource('local x <const> = 2')
  await pause(1200)
  assert(editor.session.getAnnotations().length === 0, 'stale annotation')
  instance.setSource('devices.get("Desk ")')
  editor.moveCursorTo(0, 'devices.get("Desk '.length)
  const completer = editor.completers[0]
  let items: ace.Ace.Completion[] = []
  completer.getCompletions(editor, editor.session, editor.getCursorPosition(), '', (_error, value) => { items = value })
  const item = items.find(value => value.value === 'Desk "lamp"')
  assert(item, 'missing live device completion')
  completer.insertMatch!(editor, item!)
  assert(editor.getValue() === 'devices.get("Desk \\"lamp\\"")', 'incorrect quote insertion: ' + editor.getValue())
  instance.setSource('devices.get("Desk old suffix")')
  editor.moveCursorTo(0, 'devices.get("Desk '.length)
  completer.getCompletions(editor, editor.session, editor.getCursorPosition(), '', (_error, value) => { items = value })
  completer.insertMatch!(editor, items.find(value => value.value === 'Desk "lamp"')!)
  assert(editor.getValue() === 'devices.get("Desk \\"lamp\\"")', 'existing string suffix retained: ' + editor.getValue())
  assert(!ace.require('ace/snippets/lua').snippetText.includes('print(i)'), 'unsupported print snippet')
  instance.setSource('devices.li')
  editor.navigateFileEnd()
  completer.getCompletions(editor, editor.session, editor.getCursorPosition(), 'li', (_error, value) => { items = value })
  completer.insertMatch!(editor, items.find(value => value.value === 'light')!)
  assert(editor.getValue() === 'devices.light(target, state)', 'call snippet insertion')
  let saved = false
  const save = (event: KeyboardEvent) => { if (event.ctrlKey && event.key === 's') saved = true }
  window.addEventListener('keydown', save)
  editor.execCommand('gatewaySave')
  window.removeEventListener('keydown', save)
  assert(saved, 'save command')
  instance.setSource('first\nsecond\nthird')
  instance.jump(2)
  assert(editor.getSelectedText().trim() === 'second', 'line navigation')
  assert(source.includes('second'), 'source synchronization')
  const worker = (editor.session as unknown as { $worker: { $worker: Worker } }).$worker.$worker
  worker.dispatchEvent(new Event('error'))
  assert(workerFailed, 'worker failure notification')
  instance.destroy()
  assert(host.childElementCount === 0, 'editor cleanup')
  const next = attachEditor(host, '-- remounted', () => {}, () => ({ devices: [], groups: [] }), () => {})
  assert(ace.edit(host).getValue() === '-- remounted', 'remount')
  next.destroy()
  output.dataset.result = 'passed'
  output.textContent = 'PASS automation editor browser: worker, completion insertion, snippets, save, navigation, cleanup'
}
main().catch(error => { output.dataset.result = 'failed'; output.textContent = error.stack ?? String(error) })

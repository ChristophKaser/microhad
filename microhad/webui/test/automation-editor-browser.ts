import { CompletionContext, nextSnippetField, snippet } from '@codemirror/autocomplete'
import { diagnosticCount } from '@codemirror/lint'
import { attachEditor, completionSource, applyGatewayCompletion, snippetTemplate } from '../src/codemirrorAutomation'
import { completions } from '../src/automationCompletion'
import { automationSnippets } from '../src/automationSnippets'

const output = document.getElementById('result')!
const assert = (condition: unknown, message: string) => { if (!condition) throw new Error(message) }
const pause = (ms: number) => new Promise(resolve => setTimeout(resolve, ms))
async function main() {
  const host = document.getElementById('editor') as HTMLDivElement
  let source = '', workerFailed = false
  const catalog = { devices: [{ ieee_address: 'abc', id: 'different', name: 'Desk "lamp"', source: 'zigbee', kind: 'light', has_light: true }], groups: [] }
  const instance = attachEditor(host, '', value => { source = value }, () => catalog as never, () => { workerFailed = true })
  const editor = instance.view
  instance.setSource('local <const> x = 1\nglobal answer = 42')
  await pause(1000)
  assert(!workerFailed && diagnosticCount(editor.state) === 0, 'valid Lua syntax')
  instance.setSource('local x <wat> = 1')
  await pause(1000)
  assert(diagnosticCount(editor.state) > 0, 'missing syntax annotation')
  instance.setSource('local x <const> = 2')
  await pause(1000)
  assert(diagnosticCount(editor.state) === 0, 'stale annotation')
  for (const value of ['devices.get("Desk ")', 'devices.get("Desk old suffix")']) {
    instance.setSource(value)
    const to = 'devices.get("Desk '.length
    editor.dispatch({selection: {anchor: to}})
    const item = completions(value, to, catalog as never).find(item => item.value === 'Desk "lamp"')!
    assert(item, 'missing device completion')
    applyGatewayCompletion(editor, item, to, to)
    assert(editor.state.doc.toString() === String.raw`devices.get("Desk \"lamp\"")`, 'quote insertion: ' + editor.state.doc.toString())
  }
  instance.setSource('devices.li')
  const item = completions('devices.li', 10, catalog as never).find(item => item.value === 'light')!
  applyGatewayCompletion(editor, item, 8, 10)
  assert(editor.state.doc.toString() === 'devices.light(target, state)', 'call snippet')
  for (const item of automationSnippets) {
    instance.setSource('')
    snippet(snippetTemplate(item.content))(editor, {label: item.tabTrigger}, 0, 0)
    assert(!editor.state.doc.toString().includes('${'), 'unexpanded snippet')
    const defaults = [...item.content.matchAll(/\$\{[1-9]:(.*?)\}/g)].map(match => match[1])
    for (let index = 0; index < defaults.length; index++) {
      const selection = editor.state.selection.main
      assert(editor.state.sliceDoc(selection.from, selection.to) === defaults[index], 'snippet field: ' + item.tabTrigger)
      nextSnippetField(editor)
    }
  }
  instance.setSource('-- devices.')
  assert(completionSource(() => catalog as never)(new CompletionContext(editor.state, editor.state.doc.length, true)) === null, 'comment completion')
  let saved = false
  const save = (event: KeyboardEvent) => { if (event.ctrlKey && event.key === 's') saved = true }
  window.addEventListener('keydown', save)
  editor.contentDOM.dispatchEvent(new KeyboardEvent('keydown', { key: 's', ctrlKey: true, bubbles: true }))
  window.removeEventListener('keydown', save)
  assert(saved, 'save command')
  instance.setSource('first\nsecond\nthird')
  instance.jump(2)
  assert(editor.state.sliceDoc(editor.state.selection.main.from, editor.state.selection.main.to).trim() === 'second', 'line navigation')
  assert(source.includes('second'), 'source synchronization')
  instance.destroy()
  assert(host.childElementCount === 0, 'editor cleanup')
  const next = attachEditor(host, '-- remounted', () => {}, () => ({ devices: [], groups: [] }), () => {})
  assert(next.view.state.doc.toString() === '-- remounted', 'remount')
  next.destroy()
  output.dataset.result = 'passed'
  output.textContent = 'PASS automation editor browser: worker, completion insertion, snippets, save, navigation, cleanup'
}
main().catch(error => { output.dataset.result = 'failed'; output.textContent = error.stack ?? String(error) })

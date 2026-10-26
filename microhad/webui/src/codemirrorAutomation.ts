/// <reference types="vite/client" />
import { EditorState } from '@codemirror/state'
import { EditorView, keymap, lineNumbers, highlightActiveLine, drawSelection } from '@codemirror/view'
import { defaultKeymap, history, historyKeymap, indentWithTab } from '@codemirror/commands'
import { StreamLanguage, syntaxTree, syntaxHighlighting, defaultHighlightStyle, indentUnit, bracketMatching, type StreamParser } from '@codemirror/language'
import { lua } from '@codemirror/legacy-modes/mode/lua'
import { autocompletion, completionKeymap, snippet, nextSnippetField, prevSnippetField, closeBrackets, closeBracketsKeymap, type CompletionContext, type Completion } from '@codemirror/autocomplete'
import { setDiagnostics, lintGutter } from '@codemirror/lint'
import workerUrl from '../node_modules/.cache/microhad-worker-lua.js?url'
import { completions, type Catalog, type Completion as GatewayCompletion } from './automationCompletion'
import { automationSnippets } from './automationSnippets'

export const snippetTemplate = (value: string) => value.replace(/\t/g, '  ')

export function applyGatewayCompletion(view: EditorView, item: GatewayCompletion, from: number, to: number) {
  if (item.stringStart !== undefined && item.delimiter) {
    from = item.stringStart
    const remaining = view.state.doc.sliceString(to, view.state.doc.lineAt(to).to)
    const closing = remaining.match(new RegExp('^(?:\\\\.|[^' + item.delimiter + '\\\\])*' + item.delimiter))
    if (closing) to += closing[0].length
    const text = item.value.replace(/\\/g, '\\\\').replace(/\n/g, '\\n').replace(/\r/g, '\\r').replace(new RegExp(item.delimiter, 'g'), '\\' + item.delimiter) + item.delimiter
    view.dispatch({ changes: { from, to, insert: text }, selection: { anchor: from + text.length } })
  } else if (item.snippet) snippet(snippetTemplate(item.snippet))(view, { label: item.caption }, from, to)
  else view.dispatch({ changes: { from, to, insert: item.value }, selection: { anchor: from + item.value.length } })
}

export function completionSource(catalog: () => Catalog) {
  return (context: CompletionContext) => {
    const source = context.state.doc.toString(), to = context.pos
    const prefix = context.matchBefore(/\w*/)
    const suggestions = completions(source, to, catalog())
    const options: Completion[] = suggestions.map(item => ({
      label: item.value, displayLabel: item.caption, detail: item.meta, info: item.docText,
      boost: Math.min(99, Math.round(item.score / 100)),
      apply: (view, _completion, from, end) => applyGatewayCompletion(view, item, from, end),
    }))
    // Gateway completion performs string/comment detection. Offer snippets only in code.
    if (!/string|comment/i.test(syntaxTree(context.state).resolveInner(Math.max(0, to - 1), -1).name) && !suggestions.some(item => item.stringStart !== undefined) && !/['".]$/.test(source.slice(0, to - (prefix?.text.length ?? 0)))) {
      options.push(...automationSnippets.filter(item => item.tabTrigger.startsWith(prefix?.text ?? '')).map(item => ({ label: item.tabTrigger, detail: item.name,
        apply: snippet(snippetTemplate(item.content)) })))
    }
    return options.length ? { from: prefix?.from ?? to, options, filter: false } : null
  }
}

export function attachEditor(element: HTMLDivElement, source: string, onChange: (value: string) => void, catalog: () => Catalog, onWorkerFailure: () => void) {
  let worker: Worker | undefined, timer: ReturnType<typeof setTimeout> | undefined, revision = 0, destroyed = false
  const failed = () => { worker?.terminate(); worker = undefined; if (!destroyed) { view.dispatch(setDiagnostics(view.state, [])); onWorkerFailure() } }
  const check = () => {
    revision++
    if (timer) clearTimeout(timer)
    view.dispatch(setDiagnostics(view.state, []))
    timer = setTimeout(() => { try { worker?.postMessage({ id: revision, source: view.state.doc.toString() }) } catch { failed() } }, 300)
  }
  const view = new EditorView({ parent: element, state: EditorState.create({ doc: source, extensions: [
    lineNumbers(), history(), drawSelection(), highlightActiveLine(), bracketMatching(), closeBrackets(), lintGutter(),
    StreamLanguage.define(lua as unknown as StreamParser<unknown>), syntaxHighlighting(defaultHighlightStyle), indentUnit.of('  '), EditorState.tabSize.of(2),
    EditorView.contentAttributes.of({ 'aria-label': 'Lua source' }),
    EditorView.theme({ '&': { height: '100%', fontSize: '14px' }, '.cm-scroller': { overflow: 'auto' } }),
    autocompletion({ override: [completionSource(catalog)] }),
    keymap.of([{ key: 'Mod-s', run: () => { window.dispatchEvent(new KeyboardEvent('keydown', { key: 's', ctrlKey: true })); return true } },
      { key: 'Tab', run: editor => {
        if (nextSnippetField(editor)) return true
        const pos = editor.state.selection.main.head, word = editor.state.doc.sliceString(0, pos).match(/\w+$/)?.[0]
        const item = automationSnippets.find(value => value.tabTrigger === word)
        if (!item) return false
        snippet(snippetTemplate(item.content))(editor, { label: item.tabTrigger }, pos - word!.length, pos); return true
      } }, { key: 'Shift-Tab', run: prevSnippetField }, ...closeBracketsKeymap, ...completionKeymap, ...defaultKeymap, ...historyKeymap, indentWithTab]),
    EditorView.updateListener.of(update => { if (update.docChanged) { onChange(update.state.doc.toString()); check() } }),
  ] }) })
  try {
    worker = new Worker(workerUrl)
    worker.onerror = failed
    worker.onmessageerror = failed
    worker.onmessage = ({ data }) => {
      if (data.id !== revision || destroyed) return
      view.dispatch(setDiagnostics(view.state, data.errors.map((error: {row: number; column: number; text: string}) => {
        const line = view.state.doc.line(Math.max(1, Math.min(view.state.doc.lines, error.row + 1)))
        const from = Math.min(line.to, line.from + error.column)
        return { from, to: Math.min(line.to, from + 1), severity: 'error' as const, message: error.text }
      })))
    }
    check()
  } catch { failed() }
  return {
    view,
    resize() { view.requestMeasure() },
    setSource(value: string) { if (view.state.doc.toString() !== value) view.dispatch({ changes: { from: 0, to: view.state.doc.length, insert: value }, selection: { anchor: 0 } }) },
    jump(number: number) { const line = view.state.doc.line(Math.max(1, Math.min(view.state.doc.lines, number))); view.dispatch({ selection: { anchor: line.from, head: line.to }, scrollIntoView: true }); view.focus() },
    destroy() { destroyed = true; clearTimeout(timer); worker?.terminate(); view.destroy(); element.replaceChildren() },
  }
}

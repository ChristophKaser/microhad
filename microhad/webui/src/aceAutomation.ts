/// <reference types="vite/client" />
import * as ace from 'ace-builds'
import 'ace-builds/src-noconflict/mode-lua'
import 'ace-builds/src-noconflict/ext-language_tools'
import 'ace-builds/src-noconflict/snippets/lua'
import workerUrl from '../node_modules/.cache/microhad-worker-lua.js?url'
import { completions, type Catalog, type Completion } from './automationCompletion'

// The upstream numeric-for snippet uses print(), which the sandbox does not expose.
const luaSnippets = ace.require('ace/snippets/lua')
luaSnippets.snippetText = luaSnippets.snippetText.replace('print(i)', '-- body')

ace.config.setModuleUrl('ace/mode/lua_worker', workerUrl)
// Every used mode/snippet/worker is bundled; never infer a remote asset path.
ace.config.set('loadWorkerFromBlob', false)

export function attachEditor(element: HTMLDivElement, source: string, onChange: (value: string) => void, catalog: () => Catalog, onWorkerFailure: () => void) {
  const editor = ace.edit(element, { mode: 'ace/mode/lua', fontSize: 14, tabSize: 2, useSoftTabs: true, showPrintMargin: false,
    enableBasicAutocompletion: true, enableLiveAutocompletion: true, enableSnippets: true })
  try {
    editor.setValue(source, -1)
    editor.textInput.getElement().setAttribute('aria-label', 'Lua source')
    const custom: ace.Ace.Completer = {
      identifierRegexps: [/[a-zA-Z_0-9]/],
      triggerCharacters: ['.', '"', "'", '=', ','],
      getDocTooltip(item) { return { ...item, docText: item.docText ?? item.meta } },
      getCompletions(_editor, session, position, _prefix, callback) {
        callback(null, completions(session.getValue(), session.doc.positionToIndex(position), catalog()))
      },
      insertMatch(_editor, data) {
        const position = editor.getCursorPosition()
        const line = editor.session.getLine(position.row)
        const prefix = line.slice(0, position.column).match(/[\w]*$/)?.[0] ?? ''
        const range = new ace.Range(position.row, position.column - prefix.length, position.row, position.column)
        // A quoted value may contain spaces or punctuation: replace the whole partial string.
        const suggestion = data as Completion
        if (suggestion.stringStart !== undefined && suggestion.delimiter) {
          const start = editor.session.doc.indexToPosition(suggestion.stringStart, 0)
          range.start = start
          const delimiter = suggestion.delimiter
          const remaining = line.slice(position.column)
          const closing = remaining.match(new RegExp('^(?:\\\\.|[^' + delimiter + '\\\\])*' + delimiter))
          if (closing) range.end.column += closing[0].length
          const escaped = (data.value ?? '').replace(/\\/g, '\\\\').replace(/\n/g, '\\n').replace(/\r/g, '\\r').replace(new RegExp(delimiter, 'g'), '\\' + delimiter)
          editor.session.replace(range, escaped + delimiter)
          return
        }
        editor.session.remove(range)
        if (data.snippet) ace.require('ace/snippets').snippetManager.insertSnippet(editor, data.snippet)
        else editor.insert(data.value ?? '')
      },
    }
    const language = ace.require('ace/ext/language_tools')
    // Text/snippet providers remain available outside strings/comments; gateway values
    // must be the only provider inside strings to avoid irrelevant suggestions.
    const defaults = [language.textCompleter, language.snippetCompleter].filter(Boolean)
    editor.completers = [custom, ...defaults.map(provider => ({
      getCompletions(ed: ace.Ace.Editor, session: ace.Ace.EditSession, position: ace.Ace.Point, prefix: string, callback: ace.Ace.CompleterCallback) {
        const token = session.getTokenAt(position.row, position.column)
        if (token?.type.includes('comment') || token?.type.includes('string') || /[\w.]\.$/.test(session.getLine(position.row).slice(0, position.column - prefix.length))) callback(null, [])
        else provider.getCompletions(ed, session, position, prefix, (error: unknown, items: ace.Ace.Completion[]) => callback(error, items.filter(item => !['os','io','debug','coroutine','package','require','load','loadfile','dofile','print','collectgarbage','rawget','rawset','setmetatable','getmetatable','warn'].includes(item.value ?? ''))))
      },
    }))]
    editor.commands.addCommand({ name: 'gatewaySave', bindKey: { win: 'Ctrl-S', mac: 'Command-S' }, exec: () => window.dispatchEvent(new KeyboardEvent('keydown', { key: 's', ctrlKey: true })) })
    editor.session.on('change', () => onChange(editor.getValue()))
    editor.session.on('changeMode', () => editor.resize())
    const worker = (editor.session as unknown as { $worker?: { $worker: Worker; reportError: () => void } }).$worker
    const failed = () => { editor.session.clearAnnotations(); onWorkerFailure() }
    if (worker) {
      worker.reportError = failed
      worker.$worker.addEventListener('error', failed)
    } else onWorkerFailure()
    return {
      resize() { editor.resize(true) },
      setSource(value: string) { if (editor.getValue() !== value) editor.setValue(value, -1) },
      jump(line: number) { editor.gotoLine(line, 0, true); editor.selection.selectLine(); editor.focus() },
      destroy() { worker?.$worker?.removeEventListener('error', failed); editor.destroy(); element.replaceChildren() },
    }
  } catch (failure) { editor.destroy(); element.replaceChildren(); throw failure }
}

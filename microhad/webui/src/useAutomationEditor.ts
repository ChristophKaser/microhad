import { useEffect, useRef, useState, type RefObject } from 'react'
import type { Catalog } from './automationCompletion'
import type { attachEditor } from './codemirrorAutomation'

export function useAutomationEditor(selected: string, source: string, setSource: (value: string) => void, catalog: Catalog, textarea: RefObject<HTMLTextAreaElement>) {
  const container = useRef<HTMLDivElement>(null)
  const instance = useRef<ReturnType<typeof attachEditor> | null>(null)
  const latest = useRef({ source, catalog, setSource })
  latest.current = { source, catalog, setSource }
  const [ready, setReady] = useState(false)
  const [warning, setWarning] = useState('')
  useEffect(() => {
    let active = true
    setReady(false); setWarning('')
    if (selected) void import('./codemirrorAutomation').then(({ attachEditor }) => {
      if (!active || !container.current) return
      instance.current = attachEditor(container.current, latest.current.source, value => latest.current.setSource(value), () => latest.current.catalog,
        () => { if (active) setWarning('Live syntax checking is unavailable.') })
      setReady(true)
    }).catch(() => {
      if (!active) return
      instance.current?.destroy(); instance.current = null
      if (active) setWarning('The enhanced editor is unavailable. You can continue editing below.')
    })
    return () => { active = false; instance.current?.destroy(); instance.current = null }
  }, [selected])
  useEffect(() => { instance.current?.setSource(source) }, [source])
  useEffect(() => { if (ready) instance.current?.resize() }, [ready])
  return { container, ready, warning, jump(line: number) {
    if (instance.current) instance.current.jump(line)
    else if (textarea.current) {
      const start = source.split('\n').slice(0, line - 1).reduce((length, text) => length + text.length + 1, 0)
      textarea.current.focus(); textarea.current.setSelectionRange(start, start + (source.split('\n')[line - 1]?.length ?? 0))
    }
  } }
}

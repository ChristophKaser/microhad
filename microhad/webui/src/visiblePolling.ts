/** Poll passive UI data only while visible; active operations use their own monitors. */
export function startVisiblePolling(refresh: () => void | Promise<unknown>, interval: number): () => void {
  let stopped = false
  let pending = false
  let timer: number | undefined
  const clear = () => { window.clearTimeout(timer); timer = undefined }
  const poll = async () => {
    if (stopped || document.hidden || pending) return
    pending = true
    try { await refresh() }
    // Callers report request errors; an unexpected rejection must not stop polling.
    catch { /* Keep future refreshes scheduled. */ }
    finally {
      pending = false
      if (!stopped && !document.hidden) timer = window.setTimeout(() => { void poll() }, interval)
    }
  }
  const visibility = () => {
    clear()
    if (!document.hidden) void poll()
  }
  document.addEventListener('visibilitychange', visibility)
  void poll()
  return () => { stopped = true; clear(); document.removeEventListener('visibilitychange', visibility) }
}

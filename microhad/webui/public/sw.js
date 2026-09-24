self.addEventListener('push', (event) => {
  let data = { title: 'MicroHAD', body: '' }
  if (event.data) {
    try { data = event.data.json() } catch (_) { data.body = event.data.text() }
  }
  event.waitUntil(self.registration.showNotification(data.title || 'MicroHAD', { body: data.body || '' }))
})

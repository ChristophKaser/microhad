const http = require('node:http');
const { spawn } = require('node:child_process');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const esbuild = require('esbuild');
async function main() {
  const bundle = await esbuild.build({ entryPoints: [path.join(__dirname, 'automation-editor-browser.ts')],
    bundle: true, write: false, platform: 'browser', jsx: 'automatic', define: { 'process.env.NODE_ENV': '"production"' }, plugins: [{ name: 'local-worker-url', setup(build) { build.onResolve({ filter: /microhad-worker-lua\.js\?url$/ }, () => ({ path: 'worker-url', namespace: 'local-worker' })); build.onLoad({ filter: /.*/, namespace: 'local-worker' }, () => ({ contents: 'export default "/worker-lua.js"', loader: 'js' })); } }] });
  const server = http.createServer((req, res) => {
    if (req.url === '/worker-lua.js') { res.setHeader('Content-Type', 'application/javascript'); fs.readFile(path.join(__dirname, '../node_modules/.cache/microhad-worker-lua.js')).then(data => res.end(data)); }
    else if (req.url === '/bundle.js') { res.setHeader('Content-Type', 'application/javascript'); res.end(bundle.outputFiles[0].text); }
    else { res.setHeader('Content-Type', 'text/html'); res.end('<!doctype html><div id="editor" style="height:400px"></div><pre id="result"></pre><script src="/bundle.js"></script>'); }
  });
  const profile = await fs.mkdtemp(path.join(os.tmpdir(), 'automation-editor-browser-'));
  let child;
  try {
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
    child = spawn(process.env.CHROMIUM || 'chromium', ['--headless', '--no-sandbox', '--disable-gpu', '--disable-dev-shm-usage',
      '--no-first-run', '--disable-background-networking', `--user-data-dir=${profile}`, '--remote-debugging-pipe'], { stdio: ['ignore', 'ignore', 'pipe', 'pipe', 'pipe'] });
    let nextId = 0, buffer = '', errors = '';
    const pending = new Map();
    const requests = [];
    child.stderr.on('data', data => { errors = (errors + data).slice(-2000); });
    child.stdio[4].on('data', data => {
      buffer += data.toString();
      let end;
      while ((end = buffer.indexOf('\0')) !== -1) {
        const message = JSON.parse(buffer.slice(0, end)); buffer = buffer.slice(end + 1);
        if (message.method === 'Network.requestWillBeSent') requests.push(message.params.request.url);
        const waiter = pending.get(message.id);
        if (waiter) { pending.delete(message.id); message.error ? waiter.reject(new Error(JSON.stringify(message.error))) : waiter.resolve(message.result); }
      }
    });
    const send = (method, params = {}, sessionId) => new Promise((resolve, reject) => {
      const id = ++nextId; pending.set(id, { resolve, reject });
      child.stdio[3].write(JSON.stringify({ id, method, params, ...(sessionId ? { sessionId } : {}) }) + '\0');
    });
    const deadline = setTimeout(() => { child.kill('SIGKILL'); for (const waiter of pending.values()) waiter.reject(new Error('Browser timeout: ' + errors)); }, 30000);
    try {
      const { targetId } = await send('Target.createTarget', { url: 'about:blank' });
      const { sessionId } = await send('Target.attachToTarget', { targetId, flatten: true });
      await send('Network.enable', {}, sessionId);
      await send('Page.navigate', { url: `http://127.0.0.1:${server.address().port}/` }, sessionId);
      for (;;) {
        const result = await send('Runtime.evaluate', { expression: `JSON.stringify({ state: document.getElementById('result')?.dataset.result, text: document.getElementById('result')?.textContent })`, returnByValue: true }, sessionId);
        const value = JSON.parse(result.result.value || '{}');
        if (value.state === 'failed') throw new Error(value.text);
        if (value.state === 'passed') {
          if (!requests.some(url => url.endsWith('/worker-lua.js')) || requests.some(url => !url.startsWith(`http://127.0.0.1:${server.address().port}/`) && !url.startsWith('data:'))) throw new Error('Unexpected editor asset requests: ' + JSON.stringify(requests));
          console.log(value.text + '; all assets local'); break;
        }
        await new Promise(resolve => setTimeout(resolve, 100));
      }
    } finally { clearTimeout(deadline); }

  } finally {
    if (child && child.exitCode === null && child.signalCode === null) { const closed = new Promise(resolve => child.once('close', resolve)); child.kill('SIGKILL'); await closed; }
    server.closeAllConnections(); await new Promise(resolve => server.close(resolve));
    await fs.rm(profile, { recursive: true, force: true });
  }
}
main().catch(error => { console.error(error.message); process.exitCode = 1; });

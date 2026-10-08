const http = require('node:http');
const { spawn } = require('node:child_process');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const esbuild = require('esbuild');
async function main() {
  const bundle = await esbuild.build({ entryPoints: [path.join(__dirname, 'gateway-settings-browser.tsx')],
    bundle: true, write: false, platform: 'browser', jsx: 'automatic', define: { 'process.env.NODE_ENV': '"production"' } });
  const server = http.createServer((req, res) => {
    if (req.url === '/bundle.js') { res.setHeader('Content-Type', 'application/javascript'); res.end(bundle.outputFiles[0].text); }
    else { res.setHeader('Content-Type', 'text/html'); res.end('<!doctype html><div id="root"></div><pre id="result"></pre><script src="/bundle.js"></script>'); }
  });
  const profile = await fs.mkdtemp(path.join(os.tmpdir(), 'gateway-settings-browser-'));
  let child;
  try {
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
    child = spawn(process.env.CHROMIUM || 'chromium', ['--headless', '--disable-gpu', '--disable-dev-shm-usage',
      '--no-first-run', '--disable-background-networking', `--user-data-dir=${profile}`, '--dump-dom',
      '--virtual-time-budget=15000', `http://127.0.0.1:${server.address().port}/`]);
    let output = '', errors = '';
    child.stdout.on('data', data => { output += data; });
    child.stderr.on('data', data => { errors = (errors + data).slice(-3000); });
    const timer = setTimeout(() => child.kill('SIGKILL'), 45000);
    let code;
    try { code = await new Promise((resolve, reject) => { child.on('close', resolve); child.on('error', reject); }); }
    finally { clearTimeout(timer); }
    if (code !== 0 || !output.includes('data-result="passed"')) throw new Error(output + errors);
    console.log(output.match(/PASS gateway [^<]+/)[0]);
  } finally {
    if (child && child.exitCode === null) child.kill('SIGKILL');
    server.closeAllConnections(); await new Promise(resolve => server.close(resolve));
    await fs.rm(profile, { recursive: true, force: true });
  }
}
main().catch(error => { console.error(error.message); process.exitCode = 1; });

/** Exercise the production React component in real headless Chromium and HTTP. */
const http = require('node:http');
const { spawn } = require('node:child_process');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const esbuild = require('esbuild');

async function main() {
  const build = await esbuild.build({ entryPoints: [path.join(__dirname, 'matter-devices-browser.tsx')],
    bundle: true, write: false, platform: 'browser', jsx: 'automatic', define: { 'process.env.NODE_ENV': '"production"' } });
  let mode = 'normal', posts = 0, nextJob = 1, operationValue = false, operationLevel = 0;
  const jobs = new Map();
  const server = http.createServer(async (request, response) => {
    let body = '';
    for await (const chunk of request) body += chunk;
    response.setHeader('Content-Type', 'application/json');
    const send = value => response.end(JSON.stringify(value));
    if (request.url === '/') {
      response.setHeader('Content-Type', 'text/html');
      response.end('<!doctype html><html><body><div id="root"></div><pre id="result"></pre><script src="/bundle.js"></script></body></html>');
    } else if (request.url === '/bundle.js') {
      response.setHeader('Content-Type', 'application/javascript'); response.end(build.outputFiles[0].text);
    } else if (request.url === '/fixture/mode') { mode = body; posts = 0; send({ ok: true }); }
    else if (request.url === '/fixture/count') send({ posts });
    else if (request.url === '/api/matter/devices/snapshot') {
      posts++; const fields = JSON.parse(body), job = nextJob++;
      const endpoint = mode === 'duplicate' ? 0 : fields.cursor;
      const device = { node: '0000000000000002', state: 'complete', online: mode !== 'offline',
        id: `matter:0000000000000001:0000000000000002:${endpoint.toString(16).padStart(4, '0')}`,
        endpoint, servers: endpoint === 0 ? [29] : [6, 8, 29] };
      const result = { revision: '0000000000000007', devices: mode === 'empty' ? [] : [mode === 'recovery' ?
        { ...device, state: 'recovery_required', online: false, id: null, endpoint: null, servers: [] } : device],
        next_cursor: mode === 'empty' || mode === 'recovery' || fields.cursor === 1 ? null : 1 };
      jobs.set(job, mode === 'pending' ? { job, state: 'running' } : { job, state: 'complete', response:
        mode === 'stale' ? { schema: 1, ok: false, error: 'stale_revision' } : { schema: 1, ok: true, result } });
      response.statusCode = 202; send({ schema: 1, ok: true, job, state: 'queued' });
    } else if (request.url === '/api/matter/devices/read' || request.url === '/api/matter/devices/command') {
      const fields = JSON.parse(body), job = nextJob++;
      if (request.url.endsWith('/command')) {
        if (fields.command === 'on') operationValue = true;
        if (fields.command === 'off') operationValue = false;
        if (fields.command === 'toggle') operationValue = !operationValue;
        if (fields.command === 'move_to_level') operationLevel = fields.level;
      }
      const level = fields.cluster === 8 || fields.command === 'move_to_level';
      jobs.set(job, { job, state: 'complete', response: { schema: 1, ok: true, result: {
        state: 'complete', job: job.toString(16).padStart(16, '0'), result: { node: fields.node, endpoint: fields.endpoint,
          cluster: level ? 8 : 6, attribute: 0, value: level ? { type: 'u8', value: operationLevel } :
            { type: 'bool', value: operationValue } } } } });
      response.statusCode = 202; send({ schema: 1, ok: true, job, state: 'queued' });
    } else if (request.url.startsWith('/api/matter/jobs/')) send(jobs.get(Number(request.url.split('/').pop())));
    else { response.statusCode = 404; send({ ok: false }); }
  });
  const profile = await fs.mkdtemp(path.join(os.tmpdir(), 'matter-device-browser-'));
  let child;
  try {
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
    child = spawn(process.env.CHROMIUM || 'chromium', ['--headless', '--disable-gpu', '--disable-dev-shm-usage',
      '--no-first-run', '--disable-background-networking', `--user-data-dir=${profile}`,
      '--dump-dom', '--virtual-time-budget=30000', `http://127.0.0.1:${server.address().port}/`]);
    let output = '', errors = '';
    child.stdout.on('data', value => { output += value; });
    child.stderr.on('data', value => { errors = (errors + value).slice(-3000); });
    const timer = setTimeout(() => child.kill('SIGKILL'), 55000);
    let exitCode;
    try { exitCode = await new Promise((resolve, reject) => { child.on('close', resolve); child.on('error', reject); }); }
    finally { clearTimeout(timer); }
    if (exitCode !== 0 || !output.includes('data-result="passed"')) throw new Error(output + errors);
    console.log(output.match(/PASS rendered[^<]+/)[0]);
  } finally {
    if (child && child.exitCode === null) child.kill('SIGKILL');
    server.closeAllConnections(); await new Promise(resolve => server.close(resolve));
    await fs.rm(profile, { recursive: true, force: true });
  }
}
main().catch(error => { console.error(error.message); process.exitCode = 1; });

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
  let mode = 'backlog', posts = 0, retryPosts = 0, nextJob = 1, operationValue = false, operationLevel = 0;
  const jobs = new Map();
  let refreshed = false, listingPosts = 0, statusReads = 0;
  const server = http.createServer(async (request, response) => {
    let body = '';
    for await (const chunk of request) body += chunk;
    response.setHeader('Content-Type', 'application/json');
    const send = value => response.end(JSON.stringify(value));
    if (request.url === '/') {
      response.setHeader('Content-Type', 'text/html');
      response.end(`<!doctype html><html><body><div id="root"></div><pre id="result"></pre><script>
        Object.defineProperty(navigator, 'bluetooth', { value: { requestDevice: async () => ({}) } });
        Object.defineProperty(navigator, 'mediaDevices', { value: { getUserMedia: async () => new MediaStream() } });
        Object.defineProperty(HTMLMediaElement.prototype, 'readyState', { get: () => 2 });
        HTMLMediaElement.prototype.play = async () => {};
        window.BarcodeDetector = class { async detect() { return [{ rawValue: 'MT:Y.K9042C00KA0648G00' }]; } };
      </script><script src="/bundle.js"></script></body></html>`);
    } else if (request.url === '/bundle.js') {
      response.setHeader('Content-Type', 'application/javascript'); response.end(build.outputFiles[0].text);
    } else if (request.url === '/fixture/mode') { mode = body; posts = 0; retryPosts = 0; send({ ok: true }); }
    else if (request.url === '/fixture/count') send({ posts });
    else if (request.url === '/fixture/retry-count') send({ retryPosts });
    else if (request.url === '/api/matter/devices/snapshot') { listingPosts++; response.statusCode=404; send({ok:false}); }
    else if (request.url === '/api/status') {
      statusReads++;
      const node='matter:0000000000000001:0000000000000002';
      send({devices:[{
        id:node, source:'matter', external_id:node, component:'node', ieee_address:'',
        name:'Matter light', kind:'light', online:true, matter_node:'0000000000000002', matter_endpoint:1,
        matter_servers:[6,8,29,91,1026,1029,1037,1066], matter_clients:[], endpoint_metadata_state:'ready',
        endpoint_descriptors:[{endpoint:0,id:node+':0000',input_clusters:[29],output_clusters:[]},
          {endpoint:1,id:node+':0001',input_clusters:[6,8,29,91,1026,1029,1037,1066],output_clusters:[]}],
        has_light:true, has_brightness:true, light_known:operationValue, light_on:operationValue,
        brightness_known:operationLevel>0, brightness_percent:Math.round(operationLevel*100/254),
        has_temperature:true, temperature_known:refreshed, temperature_celsius:23.15,
        has_humidity:true, humidity_known:refreshed, humidity_percent:52.34,
        has_air_quality:true, air_quality_known:refreshed, air_quality:'moderate',
        has_co2:true, co2_known:refreshed, co2_ppm:758.5, co2_unit:'ppm',
        has_pm25:true, pm25_known:refreshed, pm25_micrograms_m3:12.75, pm25_unit:'ug/m3',
        sensor_values: Object.fromEntries(Object.entries({air_quality:['moderate',''],co2:[758.5,'ppm'],pm25:[12.75,'ug/m3']})
          .map(([key,[value,unit]])=>[key,{value:refreshed?value:null,unit,quality:refreshed?'measured':'unknown'}])),
        value_alerts:{}, contact_initial_alert_minutes:10, contact_reminder_alert_minutes:5,
      }],scene_groups:[],scene_bindings:[],native_bindings:[]});
    } else if (request.url === '/api/matter/status') {
      send({ enabled: true, available: true, controller_initialized: true, fabric_ready: true, state: 'running' });
    } else if (request.url === '/api/thread/status') {
      send({ enabled: true, available: true, configured: true, attached: mode !== 'thread-error', linux_ipv6_or_tun: false,
        state: mode === 'thread-error' ? 'error' : 'attached', zigbee_channel: 15, dataset_channel: 15 });
    } else if (request.url === '/api/thread/retry') {
      posts++; retryPosts++; const job = nextJob++;
      jobs.set(job, { job, state: 'complete', response: { schema: 1, ok: true, result: {
        enabled: true, available: true, configured: true, attached: true, state: 'attached' } } });
      response.statusCode = 202; send({ schema: 1, ok: true, job, state: 'queued' });
    } else if (request.url === '/api/matter/devices/read' || request.url === '/api/matter/devices/command') {
      const fields = JSON.parse(body), job = nextJob++;
      if (request.url.endsWith('/command')) {
        if (fields.command === 'on') operationValue = true;
        if (fields.command === 'off') operationValue = false;
        if (fields.command === 'toggle') operationValue = !operationValue;
        if (fields.command === 'move_to_level') operationLevel = fields.level;
      }
      if (request.url.endsWith('/read')) refreshed=true;
      const cluster = fields.cluster || (fields.command === 'move_to_level' ? 8 : 6);
      let typed;
      if (cluster === 8) typed = { type: 'u8', value: operationLevel };
      else if (cluster === 6) typed = { type: 'bool', value: operationValue };
      else if (cluster === 91) typed = { type: 'air_quality', value: { value: 'moderate', quality: 'measured' } };
      else {
        const sensor = { 1026: [23.15, 'celsius'], 1029: [52.34, 'percent'], 1037: [758.5, 'ppm'], 1066: [12.75, 'ug/m3'] }[cluster];
        typed = { type: 'measurement', value: { value: sensor[0], unit: sensor[1], quality: 'measured' } };
      }
      jobs.set(job, { job, state: 'complete', response: { schema: 1, ok: true, result: {
        state: 'complete', job: job.toString(16).padStart(16, '0'), result: { node: fields.node, endpoint: fields.endpoint,
          cluster, attribute: 0, value: typed } } } });
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
    if (listingPosts !== 0 || statusReads < 2) throw new Error('inventory must use ordinary status only');
    console.log(output.match(/PASS Matter endpoints[^<]+/)[0]);
  } finally {
    if (child && child.exitCode === null) child.kill('SIGKILL');
    server.closeAllConnections(); await new Promise(resolve => server.close(resolve));
    await fs.rm(profile, { recursive: true, force: true });
  }
}
main().catch(error => { console.error(error.message); process.exitCode = 1; });

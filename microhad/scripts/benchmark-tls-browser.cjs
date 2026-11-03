// PLAYWRIGHT_MODULE can point to an existing Playwright installation.
const { chromium } = require(process.env.PLAYWRIGHT_MODULE || 'playwright');
(async () => {
  const browser = await chromium.launch({headless: true,
    ...(process.env.PLAYWRIGHT_CHROMIUM ? {executablePath: process.env.PLAYWRIGHT_CHROMIUM} : {}),
    args: ['--no-sandbox', '--disable-background-networking']});
  const report = {version: browser.version(), contexts: []};
  try {
    for (let fresh = 0; fresh < 2; fresh++) {
      const context = await browser.newContext({ignoreHTTPSErrors: true});
      await context.route('**/favicon.ico', route => route.abort());
      const page = await context.newPage();
      const response = await page.goto(process.argv[2], {waitUntil: 'load', timeout: 30000});
      const security = await response.securityDetails();
      const statuses = await page.evaluate(async count => {
        const results = [];
        for (let i = 0; i < count; i++) {
          const response = await fetch('/browser/' + i, {cache: 'no-store'});
          await response.arrayBuffer(); results.push(response.status);
        }
        return results;
      }, Number(process.argv[3]));
      if (response.status() !== 200 || statuses.some(status => status !== 200))
        throw new Error('Unexpected benchmark HTTP status');
      report.contexts.push({fresh, security, statuses});
      await context.close();
    }
  } finally { await browser.close(); }
  console.log(JSON.stringify(report));
})().catch(error => {console.error(String(error)); process.exitCode = 1;});

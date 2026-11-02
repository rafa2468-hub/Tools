// Smoke test against the live GitHub Pages URL: loads, no errors, offline worker takes over,
// opens with no network, manifest and icon reachable. Usage: node live-smoke.js <chrome.exe> [url]
const { chromium } = require('playwright-core');
const URL = process.argv[3] || 'https://rafa2468-hub.github.io/Tools/lift-log/';
let fail = 0; const ok = (c, n, x) => { console.log(c ? 'PASS' : 'FAIL', n, c ? '' : JSON.stringify(x || '')); if (!c) fail++; };
(async () => {
  const b = await chromium.launch({ executablePath: process.argv[2], headless: true });
  const ctx = await b.newContext({ viewport: { width: 390, height: 844 }, isMobile: true, hasTouch: true,
    userAgent: 'Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Mobile/15E148 Safari/604.1' });
  const p = await ctx.newPage(); const errs = [];
  p.on('pageerror', e => errs.push(String(e))); p.on('console', m => { if (m.type() === 'error') errs.push(m.text()); });
  const r = await p.goto(URL);
  ok(r.status() === 200, 'page 200', r.status());
  ok(await p.locator('#title').innerText() === 'Today', 'app renders');
  const man = await p.evaluate(async () => (await fetch('manifest.webmanifest')).json());
  ok(man.name === 'Lift Log' && man.start_url === './', 'manifest served');
  ok(await p.evaluate(async () => (await fetch('icon-180.png')).ok), 'touch icon served');
  await p.evaluate(() => navigator.serviceWorker.ready);
  await p.reload();
  ok(await p.evaluate(() => !!navigator.serviceWorker.controller), 'offline worker in control');
  await ctx.setOffline(true); await p.reload();
  ok(await p.locator('#title').innerText() === 'Today', 'opens offline');
  ok(errs.length === 0, 'no page errors', errs);
  await b.close(); process.exit(fail ? 1 : 0);
})().catch(e => { console.error(e); process.exit(2); });

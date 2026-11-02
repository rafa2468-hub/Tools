// Upgrade + day-reorder test. Serves an OLD build first (with saved data on the "phone"),
// then swaps the server to the NEW build, the way a GitHub Pages deploy does, and checks:
// the data survives, the update arrives within two opens, and days reorder with the arrows.
// Usage: node upgrade.js <old app folder> <new app folder> <chrome.exe>
const http = require('http'), fs = require('fs'), path = require('path');
const { chromium } = require('playwright-core');
const OLD = path.resolve(process.argv[2]), NEW = path.resolve(process.argv[3]), CHROME = process.argv[4];
const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.png': 'image/png', '.webmanifest': 'application/manifest+json' };
let serveDir = OLD, pass = 0, fail = 0;
const verOf = dir => (fs.readFileSync(path.join(dir, 'index.html'), 'utf8').match(/APP_VERSION = '([^']+)'/) || [])[1];
const OLD_V = verOf(OLD), NEW_V = verOf(NEW);
if (!OLD_V || !NEW_V || OLD_V === NEW_V) { console.error('need two different builds', OLD_V, NEW_V); process.exit(2); }
const ok = (c, n, x) => { if (c) { pass++; console.log('PASS', n); } else { fail++; console.log('FAIL', n, JSON.stringify(x === undefined ? '' : x)); } };
const server = http.createServer((req, res) => {
  let p = decodeURIComponent(req.url.split('?')[0]).replace(/^\/app\//, '/'); if (p.endsWith('/')) p += 'index.html';
  const f = path.join(serveDir, p);
  if (!f.startsWith(serveDir) || !fs.existsSync(f)) { res.writeHead(404); res.end(); return; }
  res.writeHead(200, { 'Content-Type': TYPES[path.extname(f)] || 'application/octet-stream', 'Cache-Control': 'no-cache' });
  fs.createReadStream(f).pipe(res);
});

// Maya-like saved data: four days in her current order, one finished Push workout.
const day = (id, name, ex) => ({ id, name, items: ex.map(e => ({ exId: e, sets: 3, reps: '' })) });
const seed = {
  v: 1, unit: 'lb', exercises: [], splits: [{ id: 'sp1', name: 'Push / Pull / Legs', days: [
    day('d-push', 'Push', ['d-barbell-overhead-press', 'd-cable-fly']),
    day('d-pull', 'Pull', ['d-lat-pulldown', 'd-face-pull']),
    day('d-quads', 'Quads/Hams', ['d-leg-extension', 'd-leg-press']),
    day('d-glutes', 'Glutes', ['d-hip-thrust', 'd-back-extension'])] }],
  activeSplitId: 'sp1', active: null, lastBackup: null, hideInstall: true,
  sessions: [{ id: 's1', dayId: 'd-push', splitId: 'sp1', name: 'Push', start: Date.now() - 3600e3, end: Date.now() - 1800e3, unit: 'lb', notes: '', entries: [{ exId: 'd-cable-fly', name: 'Cable Fly', sets: [{ w: 30, r: 12 }] }] }]
};

(async () => {
  await new Promise(r => server.listen(0, '127.0.0.1', r));
  const URL = `http://127.0.0.1:${server.address().port}/app/`;
  const browser = await chromium.launch({ executablePath: CHROME, headless: true });
  const ctx = await browser.newContext({ viewport: { width: 390, height: 844 }, isMobile: true, hasTouch: true,
    userAgent: 'Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Mobile/15E148 Safari/604.1' });
  const page = await ctx.newPage(); const errors = [];
  page.on('pageerror', e => errors.push(String(e))); page.on('console', m => { if (m.type() === 'error') errors.push(m.text()); });
  const version = () => page.evaluate(() => typeof APP_VERSION === 'string' ? APP_VERSION : null);
  const state = () => page.evaluate(() => JSON.parse(localStorage.getItem('liftlog.v1')));
  const dayNames = async () => (await state()).splits[0].days.map(d => d.name).join(',');

  // 1. Old build with her data, offline worker installed
  await page.goto(URL);
  await page.evaluate(s => localStorage.setItem('liftlog.v1', JSON.stringify(s)), seed);
  await page.evaluate(() => navigator.serviceWorker.ready);
  await page.reload();
  ok(await version() === OLD_V && await page.evaluate(() => !!navigator.serviceWorker.controller), 'old ' + OLD_V + ' running with the worker in control');
  ok((await page.locator('#view').innerText()).includes('Quads/Hams'), 'old build shows her split');

  // 2. Deploy the new build; she opens the app a few times
  serveDir = NEW;
  const seen = [];
  for (let i = 0; i < 4 && (seen[seen.length - 1] !== NEW_V); i++) { await page.reload(); await page.waitForTimeout(800); seen.push(await version()); }
  ok(seen.includes(NEW_V) && seen.indexOf(NEW_V) <= 1, 'update to ' + NEW_V + ' shows up by the second open', seen);
  const st = await state();
  ok(st.sessions.length === 1 && st.splits[0].days.length === 4 && await dayNames() === 'Push,Pull,Quads/Hams,Glutes', 'her split and workout survive the update', st.splits[0].days.map(d => d.name));

  // 3. Reorder days in Plan
  await page.locator('[data-a="tab"][data-tab="plan"]').click();
  ok(await page.locator('[data-a="day-move"]').count() === 8, 'every day has up and down arrows');
  ok(await page.locator('[data-a="day-move"][data-i="0"][data-d="-1"]').isDisabled() && await page.locator('[data-a="day-move"][data-i="3"][data-d="1"]').isDisabled(), 'first day cannot go up, last cannot go down');
  await page.locator('[data-a="day-move"][data-i="1"][data-d="1"]').click();
  ok(await dayNames() === 'Push,Quads/Hams,Pull,Glutes', 'Pull down once gives Push, Quads/Hams, Pull, Glutes', await dayNames());
  ok(await page.locator('#title').innerText() === 'Plan', 'tapping an arrow does not open the day');
  await page.screenshot({ path: path.join(__dirname, 'shots', '12-plan-reorder.png'), fullPage: true });
  await page.locator('[data-a="day-move"][data-i="3"][data-d="-1"]').click();
  ok(await dayNames() === 'Push,Quads/Hams,Glutes,Pull', 'moving up works too');
  await page.locator('[data-a="day-move"][data-i="2"][data-d="1"]').click();
  ok(await dayNames() === 'Push,Quads/Hams,Pull,Glutes', 'and back');
  const stAfter = await state();
  ok(stAfter.splits[0].days.find(d => d.name === 'Pull').items.length === 2 && stAfter.sessions.length === 1, 'days keep their exercises; history untouched');
  await page.locator('.item[data-a="edit-day"]', { hasText: 'Glutes' }).first().click();
  ok(await page.locator('#title').innerText() === 'Glutes', 'tapping the row still opens the day');

  // 4. Up next follows the new order (last workout was Push)
  await page.locator('[data-a="tab"][data-tab="today"]').click();
  const next = await page.locator('.card.next h3').innerText();
  ok(next.startsWith('Quads/Hams'), 'Up next is Quads/Hams after Push', next);
  const cards = await page.locator('.card h3').allInnerTexts();
  ok(cards.filter(c => /^(Push|Quads|Pull|Glutes)/.test(c)).map(c => c.split(/\s|UP/)[0]).join(',') === 'Push,Quads/Hams,Pull,Glutes', 'Today lists days in the new order', cards);
  await page.reload();
  ok(await dayNames() === 'Push,Quads/Hams,Pull,Glutes', 'new order survives closing the app');

  // 5. One-day split shows no arrows
  await page.evaluate(() => { const s = JSON.parse(localStorage.getItem('liftlog.v1')); s.splits.push({ id: 'sp2', name: 'Full body', days: [{ id: 'fb', name: 'Full Body', items: [] }] }); localStorage.setItem('liftlog.v1', JSON.stringify(s)); });
  await page.reload();
  await page.locator('[data-a="tab"][data-tab="plan"]').click();
  ok(await page.locator('[data-a="day-move"][data-split="sp2"]').count() === 0, 'a one-day split has no arrows');

  ok(errors.length === 0, 'no page errors', errors);
  await browser.close(); server.close();
  console.log(`\n${pass} passed, ${fail} failed`);
  process.exit(fail ? 1 : 0);
})().catch(e => { console.error(e); process.exit(2); });

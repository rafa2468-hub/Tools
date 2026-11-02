// Switching between splits: "Use this", Today and Up next following the split in use,
// history kept across switches, a workout in progress surviving a switch, new split
// taking over, rename, and deleting the split in use (and the last one).
// Usage: node splits.js <app folder> <chrome.exe>
const http = require('http'), fs = require('fs'), path = require('path');
const { chromium } = require('playwright-core');
const APP = path.resolve(process.argv[2]), CHROME = process.argv[3];
const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.png': 'image/png', '.webmanifest': 'application/manifest+json' };
let pass = 0, fail = 0;
const ok = (c, n, x) => { if (c) { pass++; console.log('PASS', n); } else { fail++; console.log('FAIL', n, JSON.stringify(x === undefined ? '' : x)); } };
const server = http.createServer((req, res) => {
  let p = decodeURIComponent(req.url.split('?')[0]).replace(/^\/app\//, '/'); if (p.endsWith('/')) p += 'index.html';
  const f = path.join(APP, p);
  if (!f.startsWith(APP) || !fs.existsSync(f)) { res.writeHead(404); res.end(); return; }
  res.writeHead(200, { 'Content-Type': TYPES[path.extname(f)] || 'application/octet-stream' }); fs.createReadStream(f).pipe(res);
});

const day = (id, name, ex) => ({ id, name, items: ex.map(e => ({ exId: e, sets: 2, reps: '' })) });
const seed = {
  v: 1, unit: 'lb', exercises: [], activeSplitId: 'ppl', active: null, lastBackup: null, hideInstall: true,
  splits: [
    { id: 'ppl', name: 'My PPL', days: [day('push', 'Push', ['d-barbell-bench-press']), day('pull', 'Pull', ['d-lat-pulldown'])] },
    { id: 'sum', name: 'Summer', days: [day('upper', 'Upper', ['d-dumbbell-row']), day('lower', 'Lower', ['d-leg-press'])] }],
  sessions: [{ id: 's1', dayId: 'push', splitId: 'ppl', name: 'Push', start: Date.now() - 86400e3, end: Date.now() - 86400e3 + 3600e3, unit: 'lb', notes: '', entries: [{ exId: 'd-barbell-bench-press', name: 'Barbell Bench Press', sets: [{ w: 95, r: 10 }] }] }]
};

(async () => {
  await new Promise(r => server.listen(0, '127.0.0.1', r));
  const URL = `http://127.0.0.1:${server.address().port}/app/`;
  const browser = await chromium.launch({ executablePath: CHROME, headless: true });
  const ctx = await browser.newContext({ viewport: { width: 390, height: 844 }, isMobile: true, hasTouch: true,
    userAgent: 'Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Mobile/15E148 Safari/604.1' });
  const page = await ctx.newPage(); const errors = [], dialogs = [];
  page.on('pageerror', e => errors.push(String(e))); page.on('console', m => { if (m.type() === 'error') errors.push(m.text()); });
  page.on('dialog', d => { dialogs.push(d.message()); d.accept(); });
  const state = () => page.evaluate(() => JSON.parse(localStorage.getItem('liftlog.v1')));
  const tab = t => page.locator(`[data-a="tab"][data-tab="${t}"]`).click();
  const todayDays = async () => (await page.locator('#view .card h3').allInnerTexts()).map(t => t.split(/\s*UP NEXT/i)[0].trim()).filter(t => ['Push', 'Pull', 'Upper', 'Lower', 'Legs', 'Full Body'].includes(t));
  const upNext = async () => (await page.locator('.card.next').count()) ? (await page.locator('.card.next h3').innerText()).split(/\s*UP NEXT/i)[0].trim() : null;
  const splitCard = name => page.locator('#view .card', { has: page.locator('h3', { hasText: name }) }).first();

  await page.goto(URL);
  await page.evaluate(s => localStorage.setItem('liftlog.v1', JSON.stringify(s)), seed);
  await page.reload();

  // 1. Starting point: My PPL in use
  ok((await todayDays()).join() === 'Push,Pull' && await upNext() === 'Pull', 'Today shows My PPL, Pull up next', [await todayDays(), await upNext()]);
  await tab('plan');
  ok(await splitCard('My PPL').locator('.tag.acc').count() === 1 && await splitCard('Summer').locator('[data-a="use-split"]').count() === 1, 'Plan: My PPL in use, Summer has Use this');

  // 2. Switch to Summer
  await splitCard('Summer').locator('[data-a="use-split"]').click();
  let st = await state();
  ok(st.activeSplitId === 'sum', 'Use this makes Summer the split in use');
  ok(await splitCard('Summer').locator('.tag.acc').count() === 1 && await splitCard('My PPL').locator('[data-a="use-split"]').count() === 1 && await page.locator('.tag.acc').count() === 1, 'In use tag moves; only one split in use');
  await page.screenshot({ path: path.join(__dirname, 'shots', '13-two-splits.png'), fullPage: true });
  await tab('today');
  ok((await page.locator('#view h2').first().innerText()) === 'Summer' && (await todayDays()).join() === 'Upper,Lower', 'Today shows Summer days', await todayDays());
  ok(await upNext() === 'Upper', 'Up next starts at Upper (nothing done in Summer yet)', await upNext());
  ok(!(await page.locator('#view').innerText()).includes('last done') || (await page.locator('#view .card', { hasText: 'Upper' }).first().innerText()).indexOf('last done') < 0, 'Summer days show no last done');
  await tab('history');
  ok((await page.locator('.item[data-a="view-session"]').count()) === 1, 'History keeps the My PPL workout after the switch');

  // 3. Log a Summer workout
  await tab('today');
  await page.locator('#view .card', { hasText: 'Upper' }).first().locator('[data-a="start-day"]').click();
  await page.fill('[data-in="w"][data-e="0"][data-s="0"]', '40');
  await page.fill('[data-in="r"][data-e="0"][data-s="0"]', '10');
  await page.locator('[data-a="set-done"][data-e="0"][data-s="0"]').click();
  await page.locator('[data-a="finish"]').first().click();
  st = await state();
  ok(st.sessions.length === 2 && st.sessions.find(s => s.name === 'Upper').splitId === 'sum', 'Summer workout saved under Summer');
  await tab('today');
  ok(await upNext() === 'Lower', 'Up next moves to Lower', await upNext());

  // 4. Switch back: My PPL picks up where it left off
  await tab('plan');
  await splitCard('My PPL').locator('[data-a="use-split"]').click();
  await tab('today');
  ok((await todayDays()).join() === 'Push,Pull' && await upNext() === 'Pull', 'back on My PPL, Pull still up next', [await todayDays(), await upNext()]);
  ok((await page.locator('#view .card', { hasText: 'Push' }).first().innerText()).includes('last done'), 'Push still shows last done');
  await page.reload();
  ok((await state()).activeSplitId === 'ppl' && (await todayDays()).join() === 'Push,Pull', 'the split in use survives closing the app');

  // 5. Switching mid-workout leaves the workout alone
  await page.locator('#view .card', { hasText: 'Pull' }).first().locator('[data-a="start-day"]').click();
  await page.fill('[data-in="r"][data-e="0"][data-s="0"]', '8');
  await tab('plan');
  await splitCard('Summer').locator('[data-a="use-split"]').click();
  await tab('today');
  st = await state();
  ok(await page.locator('#title').innerText() === 'Pull' && st.active && st.active.splitId === 'ppl' && st.active.entries[0].sets[0].r === '8', 'workout in progress survives a split switch');
  await page.locator('[data-a="finish"]').first().click();
  st = await state();
  ok(st.sessions.find(s => s.name === 'Pull').splitId === 'ppl' && st.activeSplitId === 'sum', 'it is saved under the split it was started from');
  await tab('today');
  ok(await upNext() === 'Lower', 'Summer up next unaffected by the My PPL workout', await upNext());

  // 6. A new split takes over
  await tab('plan');
  await page.locator('[data-a="new-split"]').click();
  await page.fill('#sheetText', 'Glute focus');
  await page.locator('[data-a="tpl"][data-t="blank"]').click();
  await page.locator('[data-a="create-split"]').click();
  st = await state();
  const gf = st.splits.find(s => s.name === 'Glute focus');
  ok(gf && st.activeSplitId === gf.id && gf.days.length === 0, 'a new split becomes the one in use');
  await tab('today');
  ok((await page.locator('#view').innerText()).includes('This split has no days yet'), 'Today says the new split has no days');

  // 7. Rename through the ••• menu
  await tab('plan');
  await splitCard('Summer').locator('[data-a="split-menu"]').click();
  await page.locator('.menu button', { hasText: 'Rename split' }).click();
  await page.waitForSelector('#sheetText');
  await page.fill('#sheetText', 'Summer cut');
  await page.locator('[data-a="sheet-save"]').click();
  ok((await state()).splits.some(s => s.id === 'sum' && s.name === 'Summer cut'), 'rename works');

  // 8. Delete the split in use: falls back to another, history kept
  await splitCard('Glute focus').locator('[data-a="split-menu"]').click();
  dialogs.length = 0;
  await page.locator('.menu button', { hasText: 'Delete split' }).click();
  st = await state();
  ok(dialogs[0] && dialogs[0].includes('Past workouts stay in History') && !st.splits.some(s => s.name === 'Glute focus'), 'delete asks first, then removes it', dialogs);
  ok(st.activeSplitId === 'ppl' && st.sessions.length === 3, 'another split takes over; all 3 workouts kept', [st.activeSplitId, st.sessions.length]);

  // 9. Delete every split: back to the welcome card, history still there
  for (const name of ['My PPL', 'Summer cut']) { await splitCard(name).locator('[data-a="split-menu"]').click(); await page.locator('.menu button', { hasText: 'Delete split' }).click(); }
  st = await state();
  ok(st.splits.length === 0 && st.activeSplitId === null && st.sessions.length === 3, 'no splits left, none in use, history intact');
  await tab('today');
  ok((await page.locator('#view').innerText()).includes('Build my split'), 'Today offers to build a split again');
  await tab('history');
  ok(await page.locator('.item[data-a="view-session"]').count() === 3, 'History still lists all 3 workouts');

  ok(errors.length === 0, 'no page errors', errors);
  await browser.close(); server.close();
  console.log(`\n${pass} passed, ${fail} failed`);
  process.exit(fail ? 1 : 0);
})().catch(e => { console.error(e); process.exit(2); });

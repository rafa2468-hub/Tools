// End-to-end test for Lift Log. Serves the app folder over http://127.0.0.1, drives it in
// headless Chrome at iPhone size, and checks every main path. Screenshots go to tests/shots.
// Usage: node e2e.js <app folder> <chrome.exe>   (playwright-core must be resolvable)
const http = require('http'), fs = require('fs'), path = require('path');
const { chromium } = require('playwright-core');
const APP = path.resolve(process.argv[2]), CHROME = process.argv[3];
const SHOTS = path.join(__dirname, 'shots'); fs.mkdirSync(SHOTS, { recursive: true });
const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.png': 'image/png', '.webmanifest': 'application/manifest+json' };

let pass = 0, fail = 0;
const ok = (cond, name, extra) => { if (cond) { pass++; console.log('PASS', name); } else { fail++; console.log('FAIL', name, extra === undefined ? '' : JSON.stringify(extra)); } };

const server = http.createServer((req, res) => {
  let p = decodeURIComponent(req.url.split('?')[0]).replace(/^\/app\//, '/');
  if (p.endsWith('/')) p += 'index.html';
  const f = path.join(APP, p);
  if (!f.startsWith(APP) || !fs.existsSync(f)) { res.writeHead(404); res.end(); return; }
  res.writeHead(200, { 'Content-Type': TYPES[path.extname(f)] || 'application/octet-stream' });
  fs.createReadStream(f).pipe(res);
});

(async () => {
  await new Promise(r => server.listen(0, '127.0.0.1', r));
  const URL = `http://127.0.0.1:${server.address().port}/app/`;
  const browser = await chromium.launch({ executablePath: CHROME, headless: true });
  const ctx = await browser.newContext({
    viewport: { width: 390, height: 844 }, deviceScaleFactor: 2, isMobile: true, hasTouch: true, acceptDownloads: true,
    userAgent: 'Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Mobile/15E148 Safari/604.1'
  });
  const page = await ctx.newPage();
  const errors = [], dialogs = [];
  page.on('pageerror', e => errors.push(String(e)));
  page.on('console', m => { if (m.type() === 'error') errors.push(m.text()); });
  page.on('dialog', d => { dialogs.push(d.message()); d.accept(); });
  const text = sel => page.locator(sel).first().innerText();
  const state = () => page.evaluate(() => JSON.parse(localStorage.getItem('liftlog.v1')));
  const shot = n => page.screenshot({ path: path.join(SHOTS, n + '.png'), fullPage: true });
  const tap = sel => page.locator(sel).first().click();

  // 1. First open
  await page.goto(URL);
  ok(await text('#title') === 'Today', 'opens on Today');
  ok((await text('#view')).includes('Put it on your home screen'), 'iPhone Safari shows the home-screen hint');
  ok((await text('#view')).includes('Build my split'), 'first open asks to build a split');
  await shot('01-first-open');

  // 2. Build a PPL split
  await tap('[data-a="first-split"]');
  ok(await page.locator('#sheet.show').count() === 1, 'new split sheet opens');
  await tap('[data-a="create-split"]');
  let st = await state();
  ok(st.splits.length === 1 && st.splits[0].name === 'Push / Pull / Legs' && st.splits[0].days.map(d => d.name).join() === 'Push,Pull,Legs', 'PPL split created with 3 days', st.splits);
  ok(st.activeSplitId === st.splits[0].id, 'new split is in use');
  await shot('02-plan');

  // 3. Pick Push exercises from the list, plus one typed in
  await page.locator('.item[data-a="edit-day"]', { hasText: 'Push' }).first().click();
  ok(await text('#title') === 'Push', 'day editor opens');
  await tap('[data-a="day-add-ex"]');
  ok(await page.locator('#pickChips .chip.on').innerText() === 'Push', 'picker starts on the Push group');
  await page.locator('.pick', { hasText: 'Barbell Bench Press' }).first().click();
  await page.locator('.pick', { hasText: 'Dumbbell Lateral Raise' }).first().click();
  await page.fill('#pickSearch', 'cable y raise');
  ok((await text('#pickList')).includes('Not in the list?'), 'typing an unknown name offers to add it');
  await page.locator('[data-a="pick-create"][data-g="Push"]').click();
  ok(await page.locator('#pickDone').innerText() === 'Add 3 exercises', 'three selected');
  await shot('03-picker');
  await tap('#pickDone');
  st = await state();
  let push = st.splits[0].days[0];
  ok(push.items.length === 3, 'Push day has 3 exercises', push.items);
  ok(st.exercises.some(e => e.custom && e.name === 'Cable y raise' && e.group === 'Push'), 'typed exercise saved as her own');
  await page.locator('[data-in="item-reps"]').first().fill('8-12');
  await page.locator('[data-a="item-move"][data-i="0"][data-d="1"]').click();
  st = await state(); push = st.splits[0].days[0];
  ok(push.items[1].exId === 'd-barbell-bench-press' && push.items[1].reps === '8-12', 'reps target saved and move down works', push.items);
  await shot('04-day-editor');

  // 4. Log a workout
  await tap('[data-a="day-start"]');
  ok(await text('#title') === 'Push', 'workout started');
  ok(await page.locator('.sets tr:not(:first-child)').count() === 9, '3 exercises x 3 sets');
  ok((await text('#view')).includes('Goal 3 × 8-12'), 'goal shown');
  // bench is entry 1 after the move
  await page.fill('[data-in="w"][data-e="1"][data-s="0"]', '95');
  await page.fill('[data-in="r"][data-e="1"][data-s="0"]', '10');
  await tap('[data-a="set-done"][data-e="1"][data-s="0"]');
  ok(await page.locator('#restbar.show').count() === 1, 'rest timer shows after a set');
  await page.fill('[data-in="r"][data-e="1"][data-s="1"]', '9');
  await tap('[data-a="set-done"][data-e="1"][data-s="1"]');
  st = await state();
  ok(st.active.entries[1].sets[1].w === '95' && st.active.entries[1].sets[1].done, 'empty weight fills from the set before', st.active.entries[1].sets[1]);
  await page.fill('[data-in="w"][data-e="0"][data-s="0"]', '15');
  await page.fill('[data-in="r"][data-e="0"][data-s="0"]', '12');
  await tap('[data-a="set-done"][data-e="0"][data-s="0"]');
  // ticking with no reps and no hint is refused
  await tap('[data-a="set-done"][data-e="2"][data-s="0"]');
  st = await state();
  ok(!st.active.entries[2].sets[0].done, 'set with no reps is not ticked');
  await shot('05-workout');

  // 5. Survives closing the app mid-workout
  await page.reload();
  st = await state();
  ok(await text('#title') === 'Push' && st.active && st.active.entries[1].sets[0].w === '95', 'workout survives a reload');
  ok(await page.inputValue('[data-in="w"][data-e="1"][data-s="0"]') === '95', 'typed values come back');

  // 6. Bodyweight set added mid-workout
  await tap('[data-a="workout-add-ex"]');
  await page.locator('[data-a="pick-filter"][data-g="Pull"]').click();
  await page.locator('.pick', { hasText: /^Pull-Up/ }).first().click();
  await tap('#pickDone');
  await page.fill('[data-in="r"][data-e="3"][data-s="0"]', '6');
  await tap('[data-a="set-done"][data-e="3"][data-s="0"]');

  // 7. Finish
  dialogs.length = 0;
  await tap('[data-a="finish"]');
  ok(dialogs[0] && dialogs[0].includes('4 sets will be saved') && dialogs[0].includes('8 empty sets'), 'finish confirm counts sets', dialogs);
  st = await state();
  ok(st.active === null && st.sessions.length === 1, 'session saved');
  const s1 = st.sessions[0];
  ok(s1.entries.length === 3 && s1.entries.find(e => e.exId === 'd-pull-up').sets[0].w === null, 'bodyweight stored as no weight', s1.entries);
  const vt = await text('#view');
  ok(vt.includes('Workout saved') && vt.includes('1,985'), 'summary shows saved and volume 1,985', vt);
  ok(vt.includes('BW × 6'), 'bodyweight shown as BW');
  await shot('06-saved');

  // 8. Next time: last results and placeholders
  await tap('[data-a="tab"][data-tab="today"]');
  ok((await text('#view')).includes('last done'), 'Today shows last done');
  await shot('07-today-after');
  await tap('[data-a="start-day"]');
  ok((await text('#view')).includes('Last time: 95 lb × 10, 9'), 'shows last time for bench');
  ok(await page.getAttribute('[data-in="w"][data-e="1"][data-s="0"]', 'placeholder') === '95' && await page.getAttribute('[data-in="r"][data-e="1"][data-s="2"]', 'placeholder') === '9', 'placeholders from last time');
  await tap('[data-a="set-done"][data-e="1"][data-s="0"]');
  st = await state();
  ok(st.active.entries[1].sets[0].w === '95' && st.active.entries[1].sets[0].r === '10' && st.active.entries[1].sets[0].done, 'one tap repeats last time');
  await tap('[data-a="discard"]');
  st = await state();
  ok(st.active === null && st.sessions.length === 1, 'discard saves nothing');

  // 9. Empty workout with nothing logged
  await tap('[data-a="start-empty"]');
  dialogs.length = 0;
  await tap('[data-a="finish"]');
  ok(dialogs[0] && dialogs[0].startsWith('Nothing logged yet'), 'empty finish offers discard', dialogs);
  ok((await state()).active === null, 'empty workout discarded');

  // 10. Exercises tab and history per exercise
  await tap('[data-a="tab"][data-tab="exercises"]');
  await page.fill('[data-in="ex-search"]', 'cable y');
  ok((await text('#exlist')).includes('Cable y raise') && (await text('#exlist')).includes('MINE'), 'search finds her own exercise');
  await page.fill('[data-in="ex-search"]', 'barbell bench');
  await page.locator('.item[data-a="view-ex"]').first().click();
  const ev = await text('#view');
  ok(ev.includes('95 lb') && ev.includes('heaviest, × 10') && ev.includes('times done'), 'exercise detail shows best and history', ev);
  await shot('08-exercise');

  // 11. Escaping: a nasty name stays text
  await tap('[data-a="back-ex"]');
  await tap('[data-a="new-ex"]');
  await page.fill('#sheetText', '<img src=x onerror=alert(1)>');
  dialogs.length = 0;
  await tap('[data-a="exform-save"]');
  ok(await page.locator('img[src="x"]').count() === 0 && dialogs.length === 0 && (await text('#title')).startsWith('<img'), 'names are shown as text, never run');

  // 12. kg setting converts the hints
  await tap('[data-a="tab"][data-tab="today"]');
  await tap('[data-a="open-settings"]');
  await tap('[data-a="unit"][data-g="kg"]');
  await shot('09-settings');
  await tap('[data-a="back-today"]');
  await tap('[data-a="start-day"]');
  ok(await page.getAttribute('[data-in="w"][data-e="1"][data-s="0"]', 'placeholder') === '43', 'lb history converts to kg hint (95 lb -> 43)');
  await tap('[data-a="discard"]');

  // 13. Backup, erase, restore
  await tap('[data-a="open-settings"]');
  const [dl] = await Promise.all([page.waitForEvent('download'), tap('[data-a="export"]')]);
  const bpath = path.join(SHOTS, 'backup.json'); await dl.saveAs(bpath);
  const backup = JSON.parse(fs.readFileSync(bpath, 'utf8'));
  ok(backup.app === 'liftlog' && backup.data.sessions.length === 1 && /^lift-log-backup-\d{4}-\d\d-\d\d\.json$/.test(dl.suggestedFilename()), 'backup file has the workout', dl.suggestedFilename());
  ok((await state()).lastBackup > 0, 'last backup date recorded');
  await tap('[data-a="erase"]');
  st = await state();
  ok(st.sessions.length === 0 && st.splits.length === 0 && !st.exercises.some(e => e.custom), 'erase empties everything');
  await tap('[data-a="open-settings"]');
  await page.setInputFiles('#importFile', bpath);
  await page.waitForFunction(() => JSON.parse(localStorage.getItem('liftlog.v1')).sessions.length === 1);
  st = await state();
  ok(st.sessions.length === 1 && st.splits.length === 1 && st.exercises.some(e => e.name === 'Cable y raise'), 'restore brings it all back');
  const bad = path.join(SHOTS, 'not-a-backup.json'); fs.writeFileSync(bad, '{"hello":1}');
  dialogs.length = 0;
  await page.setInputFiles('#importFile', bad);
  await page.waitForTimeout(300);
  ok(dialogs.some(d => d.includes("isn't a Lift Log backup")) && (await state()).sessions.length === 1, 'a wrong file is refused and changes nothing', dialogs);

  // 14. Delete a workout from history
  await tap('[data-a="tab"][data-tab="history"]');
  await shot('10-history');
  await page.locator('.item[data-a="view-session"]').first().click();
  await tap('[data-a="delete-session"]');
  ok((await state()).sessions.length === 0, 'workout deleted from history');

  // 15. Offline: the service worker serves the app with no network
  await page.goto(URL);
  await page.evaluate(() => navigator.serviceWorker.ready);
  await page.reload();
  ok(await page.evaluate(() => !!navigator.serviceWorker.controller), 'service worker controls the page');
  await ctx.setOffline(true);
  await page.reload();
  ok(await text('#title') === 'Today', 'opens with no network');
  await ctx.setOffline(false);

  // 16. Dark mode screenshot
  await page.emulateMedia({ colorScheme: 'dark' });
  await tap('[data-a="tab"][data-tab="plan"]');
  await page.locator('.item[data-a="edit-day"]').first().click();
  await tap('[data-a="day-start"]');
  await shot('11-workout-dark');
  await tap('[data-a="discard"]');

  ok(errors.length === 0, 'no page errors', errors);
  await browser.close(); server.close();
  console.log(`\n${pass} passed, ${fail} failed`);
  process.exit(fail ? 1 : 0);
})().catch(e => { console.error(e); process.exit(2); });

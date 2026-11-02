// Progressive overload view (1.0.2): dated "Last time" + best while logging, per-set
// "+5 lb / +1 rep vs last time" and "New best" badges, the new-best toast, the
// "Went up vs last time" summary, and the exercise page's chart and per-session change.
// Usage: node progress.js <app folder> <chrome.exe>
const http = require('http'), fs = require('fs'), path = require('path');
const { chromium } = require('playwright-core');
const APP = path.resolve(process.argv[2]), CHROME = process.argv[3];
const SHOTS = path.join(__dirname, 'shots'); fs.mkdirSync(SHOTS, { recursive: true });
const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.png': 'image/png', '.webmanifest': 'application/manifest+json' };
let pass = 0, fail = 0;
const ok = (c, n, x) => { if (c) { pass++; console.log('PASS', n); } else { fail++; console.log('FAIL', n, JSON.stringify(x === undefined ? '' : x)); } };
const server = http.createServer((req, res) => {
  let p = decodeURIComponent(req.url.split('?')[0]).replace(/^\/app\//, '/'); if (p.endsWith('/')) p += 'index.html';
  const f = path.join(APP, p);
  if (!f.startsWith(APP) || !fs.existsSync(f)) { res.writeHead(404); res.end(); return; }
  res.writeHead(200, { 'Content-Type': TYPES[path.extname(f)] || 'application/octet-stream' }); fs.createReadStream(f).pipe(res);
});

const BENCH = 'd-barbell-bench-press', LAT = 'd-dumbbell-lateral-raise', PULL = 'd-pull-up', DAY = 864e5, now = Date.now();
const sess = (id, daysAgo, unit, entries) => ({ id, dayId: 'push', splitId: 'sp', name: 'Push', start: now - daysAgo * DAY, end: now - daysAgo * DAY + 3600e3, unit, notes: '', entries });
const e = (exId, name, sets) => ({ exId, name, sets: sets.map(([w, r]) => ({ w, r })) });
const seed = {
  v: 1, unit: 'lb', exercises: [], activeSplitId: 'sp', active: null, lastBackup: now, hideInstall: true,
  splits: [{ id: 'sp', name: 'PPL', days: [{ id: 'push', name: 'Push', items: [BENCH, LAT, PULL].map(x => ({ exId: x, sets: 3, reps: '' })) }] }],
  sessions: [
    sess('w1', 21, 'lb', [e(BENCH, 'Barbell Bench Press', [[95, 10], [95, 9], [90, 10]])]),
    sess('w2', 14, 'kg', [e(LAT, 'Dumbbell Lateral Raise', [[7, 12]]), e(PULL, 'Pull-Up', [[null, 6]])]),
    sess('w2b', 13, 'lb', [e(BENCH, 'Barbell Bench Press', [[95, 10], [95, 10], [95, 8]])]),
    sess('w3', 7, 'lb', [e(BENCH, 'Barbell Bench Press', [[100, 6], [95, 10], [95, 9]]), e(LAT, 'Dumbbell Lateral Raise', [[15, 12]]), e(PULL, 'Pull-Up', [[null, 8]])])]
};

(async () => {
  await new Promise(r => server.listen(0, '127.0.0.1', r));
  const URL = `http://127.0.0.1:${server.address().port}/app/`;
  const browser = await chromium.launch({ executablePath: CHROME, headless: true });
  const ctx = await browser.newContext({ viewport: { width: 390, height: 844 }, deviceScaleFactor: 2, isMobile: true, hasTouch: true,
    userAgent: 'Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Mobile/15E148 Safari/604.1' });
  const page = await ctx.newPage(); const errors = [];
  page.on('pageerror', x => errors.push(String(x))); page.on('console', m => { if (m.type() === 'error') errors.push(m.text()); });
  page.on('dialog', d => d.accept());
  const card = name => page.locator('#view .card', { has: page.locator('h3', { hasText: name }) }).first();
  const badges = async name => (await card(name).locator('tr.gain .badge').allInnerTexts()).map(t => t.trim().toLowerCase());
  const tick = (e, s) => page.locator(`[data-a="set-done"][data-e="${e}"][data-s="${s}"]`).click();
  const fill = async (e, s, w, r) => { if (w !== null) await page.fill(`[data-in="w"][data-e="${e}"][data-s="${s}"]`, String(w)); await page.fill(`[data-in="r"][data-e="${e}"][data-s="${s}"]`, String(r)); };
  const date7 = new Date(now - 7 * DAY).toLocaleDateString(undefined, { month: 'short', day: 'numeric' });

  await page.goto(URL);
  await page.evaluate(s => localStorage.setItem('liftlog.v1', JSON.stringify(s)), seed);
  await page.reload();
  await page.locator('[data-a="start-day"]').click();

  // 1. The header line: dated last time plus best
  const benchHead = await card('Barbell Bench Press').locator('.small.muted').first().innerText();
  ok(benchHead.includes(`Last time (${date7}): 100 lb × 6, 95 lb × 10, 95 lb × 9 · Best 100 lb × 6`), 'bench shows dated last time and best', benchHead);
  const pullHead = await card('Pull-Up').locator('.small.muted').first().innerText();
  ok(pullHead.includes(`Last time (${date7}): BW × 8`) && !pullHead.includes('Best'), 'bodyweight lift shows last time, no weight best', pullHead);

  const latHead = await card('Dumbbell Lateral Raise').locator('.small.muted').first().innerText();
  ok(latHead.includes('15 lb × 12 · Best 15.5 lb × 12'), 'best from a kg session shows in lb (7 kg = 15.5 lb)', latHead);

  // 2. Per-set badges and the new-best toast (bench is entry 0, lateral 1, pull-up 2)
  await fill(0, 0, 100, 7); await tick(0, 0);
  ok(JSON.stringify(await badges('Barbell Bench Press')) === JSON.stringify(['+1 rep vs last time', 'new best']), 'same weight, one more rep: +1 rep and New best', await badges('Barbell Bench Press'));
  ok((await page.locator('#toast').innerText()).includes('New best on Barbell Bench Press: 100 lb × 7'), 'new best toast', await page.locator('#toast').innerText());
  await tick(0, 1); // empty boxes fill from last time: 95 x 10, equal, so no badge
  ok((await badges('Barbell Bench Press')).length === 2, 'repeating last time adds no badge');
  await fill(0, 2, 100, 5); await tick(0, 2);
  ok(JSON.stringify(await badges('Barbell Bench Press')) === JSON.stringify(['+1 rep vs last time', 'new best', '+5 lb vs last time']), 'heavier than the same set last time: +5 lb, and New best stays on one set', await badges('Barbell Bench Press'));
  await fill(2, 0, null, 9); await tick(2, 0);
  ok(JSON.stringify(await badges('Pull-Up')) === JSON.stringify(['+1 rep vs last time']), 'bodyweight: +1 rep, never New best', await badges('Pull-Up'));
  await fill(1, 0, 15, 12); await tick(1, 0);
  ok((await badges('Dumbbell Lateral Raise')).length === 0, 'matching last time exactly: no badge');
  await page.screenshot({ path: path.join(SHOTS, '14-progress-workout.png'), fullPage: true });
  await tick(0, 0);
  ok(JSON.stringify(await badges('Barbell Bench Press')) === JSON.stringify(['+5 lb vs last time']), 'unticking a set removes its badges; New best needs a ticked set beating 100 x 6', await badges('Barbell Bench Press'));
  await tick(0, 0);

  // 3. Summary after finishing
  await page.locator('[data-a="finish"]').first().click();
  const sum = await page.locator('#view').innerText();
  ok(sum.includes('Went up vs last time: 2 of 3'), 'summary counts 2 of 3 lifts up', sum);
  const benchRow = await page.locator('.item', { hasText: 'Barbell Bench Press' }).first().innerText();
  ok(/100 lb × 7/.test(benchRow) && benchRow.includes(`was 100 lb × 6 on ${date7}`) && /\+1 rep/.test(benchRow) && /new best/i.test(benchRow), 'bench row: today, was, +1 rep, New best', benchRow);
  const pullRow = await page.locator('.item', { hasText: 'Pull-Up' }).first().innerText();
  ok(/BW × 9/.test(pullRow) && /\+1 rep/.test(pullRow) && !/new best/i.test(pullRow), 'pull-up row: +1 rep', pullRow);
  ok(await page.locator('.item', { hasText: 'Dumbbell Lateral Raise' }).count() === 0, 'lateral raise (no gain) not listed');
  await page.screenshot({ path: path.join(SHOTS, '15-progress-summary.png'), fullPage: true });

  // 4. Exercise page: chart and change per session
  await page.locator('[data-a="tab"][data-tab="exercises"]').click();
  await page.fill('[data-in="ex-search"]', 'barbell bench');
  await page.locator('.item[data-a="view-ex"]').first().click();
  ok(await page.locator('svg.chart circle').count() === 4, 'bench chart has 4 sessions');
  const labels = await page.locator('svg.chart text').allTextContents();
  ok(labels[0] === '100' && labels[1] === '95', 'chart scale 95 to 100 lb', labels);
  const hist = await page.locator('h2:has-text("History") + .card .item').allInnerTexts();
  const marks = hist.map(t => { const m = t.match(/(\+[\d.]+ (?:lb|kg|reps?)|−[\d.]+ (?:lb|kg|reps?)|same)\s*$/); return m ? m[1] : '-'; });
  ok(JSON.stringify(marks) === JSON.stringify(['+1 rep', '+5 lb', 'same', '-']), 'history rows: +1 rep, +5 lb, same, none', marks);
  await page.screenshot({ path: path.join(SHOTS, '16-progress-exercise.png'), fullPage: true });
  await page.locator('[data-a="back-ex"]').click();
  await page.fill('[data-in="ex-search"]', 'dumbbell lateral');
  await page.locator('.item[data-a="view-ex"]').first().click();
  const latMarks = (await page.locator('h2:has-text("History") + .card .item').allInnerTexts()).map(t => (t.match(/(\+\S+ \S+|−\S+ \S+|same)\s*$/) || [, '-'])[1]);
  ok(JSON.stringify(latMarks) === JSON.stringify(['same', '−0.5 lb', '-']), 'mixed units: 15 lb after 7 kg (15.5 lb) reads −0.5 lb', latMarks);
  await page.locator('[data-a="back-ex"]').click();
  await page.fill('[data-in="ex-search"]', 'pull-up');
  await page.locator('.item[data-a="view-ex"]', { hasText: /^Pull-Up/ }).first().click();
  ok((await page.locator('#view .card .small.muted', { hasText: 'Top set per session' }).innerText()).includes('(reps)'), 'bodyweight chart is in reps');
  await page.locator('[data-a="back-ex"]').click();
  await page.fill('[data-in="ex-search"]', 'arnold');
  await page.locator('.item[data-a="view-ex"]').first().click();
  ok(await page.locator('h2', { hasText: 'Progress' }).count() === 0 && await page.locator('svg.chart').count() === 0, 'never-done exercise: no chart');

  // 5. Dark mode look
  await page.emulateMedia({ colorScheme: 'dark' });
  await page.locator('[data-a="back-ex"]').click();
  await page.fill('[data-in="ex-search"]', 'barbell bench');
  await page.locator('.item[data-a="view-ex"]').first().click();
  await page.screenshot({ path: path.join(SHOTS, '17-progress-exercise-dark.png'), fullPage: true });

  ok(errors.length === 0, 'no page errors', errors);
  await browser.close(); server.close();
  console.log(`\n${pass} passed, ${fail} failed`);
  process.exit(fail ? 1 : 0);
})().catch(x => { console.error(x); process.exit(2); });

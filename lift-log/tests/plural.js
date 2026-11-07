// Wording (1.0.4): "1 set" / "1 minute" when the count is 1, plural otherwise, on the goal line
// while logging, the History row and the workout page stats. Fails 3 checks on 1.0.3 ("1 sets").
// 1.0.6 adds Settings > About ("1 workout", "1 exercise") and the restore confirm ("It has 1 workout");
// those fail 2 checks on 1.0.5.
// Usage: node plural.js <app folder> <chrome.exe>
const http = require('http'), fs = require('fs'), path = require('path'), os = require('os');
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
const BENCH = 'd-barbell-bench-press', FLY = 'd-cable-fly', DAY = 864e5, now = Date.now();
const seed = {
  // repsV: 1 keeps the blank Reps targets blank (1.0.7 fills them once otherwise), so "Goal N sets" is still shown
  v: 1, unit: 'lb', exercises: [], activeSplitId: 'sp', lastBackup: now, hideInstall: true, repsV: 1,
  splits: [{ id: 'sp', name: 'PPL', days: [{ id: 'push', name: 'Push', items: [{ exId: BENCH, sets: 1, reps: '' }, { exId: FLY, sets: 2, reps: '' }] }] }],
  sessions: [
    { id: 'one', dayId: 'push', splitId: 'sp', name: 'Push', start: now - 2 * DAY, end: now - 2 * DAY + 60e3, unit: 'lb', notes: '', entries: [{ exId: BENCH, name: 'Barbell Bench Press', sets: [{ w: 95, r: 8 }] }] },
    { id: 'two', dayId: 'push', splitId: 'sp', name: 'Push', start: now - 9 * DAY, end: now - 9 * DAY + 1800e3, unit: 'lb', notes: '', entries: [{ exId: BENCH, name: 'Barbell Bench Press', sets: [{ w: 95, r: 8 }, { w: 95, r: 8 }] }] }
  ],
  active: { id: 'a', dayId: 'push', splitId: 'sp', name: 'Push', start: now - 60e3, unit: 'lb', notes: '', entries: [
    { exId: BENCH, name: 'Barbell Bench Press', target: { sets: 1, reps: '' }, rest: null, sets: [{ w: '', r: '', done: false }] },
    { exId: FLY, name: 'Cable Fly', target: { sets: 2, reps: '' }, rest: null, sets: [{ w: '', r: '', done: false }, { w: '', r: '', done: false }] }] }
};
(async () => {
  await new Promise(r => server.listen(0, '127.0.0.1', r));
  const URL = `http://127.0.0.1:${server.address().port}/app/`;
  const browser = await chromium.launch({ executablePath: CHROME, headless: true });
  const ctx = await browser.newContext({ viewport: { width: 390, height: 844 }, isMobile: true, hasTouch: true });
  const page = await ctx.newPage(); const errors = [];
  page.on('pageerror', x => errors.push(String(x)));
  await page.goto(URL);
  await page.evaluate(s => localStorage.setItem('liftlog.v1', JSON.stringify(s)), seed);
  await page.reload(); await page.waitForTimeout(400);
  const text = () => page.locator('main, body').first().innerText();
  let t = await text();
  ok(/Goal 1 set ·/.test(t), 'goal line: "Goal 1 set"', t.match(/Goal[^\n]*/g));
  ok(/Goal 2 sets ·/.test(t), 'goal line: "Goal 2 sets"', t.match(/Goal[^\n]*/g));
  await page.locator('nav.tabs [data-tab="history"]').click(); await page.waitForTimeout(200);
  const row = id => page.locator(`.item[data-a="view-session"][data-id="${id}"]`).innerText();
  const r1 = await row('one'), r2 = await row('two');
  ok(/\n1 min · 1 set · 760 lb lifted\n/.test(r1), 'History row: "1 min · 1 set · 760 lb lifted"', r1);
  ok(/\n30 min · 2 sets · 1,520 lb lifted\n/.test(r2), 'History row: "30 min · 2 sets · 1,520 lb lifted"', r2);
  const stats = async id => { await page.locator(`.item[data-a="view-session"][data-id="${id}"]`).click(); await page.waitForTimeout(200);
    const s = await page.locator('.stat').allInnerTexts(); await page.locator('[data-a="back-history"]').click(); await page.waitForTimeout(200); return s.map(x => x.replace(/\s+/g, ' ').trim()); };
  const s1 = await stats('one'), s2 = await stats('two');
  ok(s1[0] === '1 minute' && s1[1] === '1 set', 'workout page: "1 minute", "1 set"', s1);
  ok(s2[0] === '30 minutes' && s2[1] === '2 sets', 'workout page: "30 minutes", "2 sets"', s2);
  // Settings > About and the restore confirm (1.0.6). Settings isn't open mid-workout, so drop the active one.
  const dialogs = []; page.on('dialog', d => { dialogs.push(d.message()); d.dismiss(); });
  await page.evaluate(s => localStorage.setItem('liftlog.v1', JSON.stringify({ ...s, active: null })), seed);
  await page.reload(); await page.waitForTimeout(400);
  const about = async () => (await page.locator('.card', { hasText: 'About' }).last().innerText()).replace(/\s+/g, ' ');
  await page.locator('[data-a="open-settings"]').click(); await page.waitForTimeout(200);
  const nEx = await page.evaluate(() => S.exercises.length);
  let a = await about();
  ok(a.includes(`2 workouts, ${nEx} exercises.`), `About: "2 workouts, ${nEx} exercises"`, a);
  await page.evaluate(() => { S.sessions = S.sessions.slice(0, 1); S.exercises = S.exercises.slice(0, 1); render(true); }); await page.waitForTimeout(200);
  a = await about();
  ok(a.includes('1 workout, 1 exercise.'), 'About: "1 workout, 1 exercise"', a);
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'liftlog-plural-'));
  const file = (name, n) => { const f = path.join(dir, name); fs.writeFileSync(f, JSON.stringify({ app: 'liftlog', exportedAt: new Date(now).toISOString(), data: { ...seed, active: null, sessions: seed.sessions.slice(0, n) } })); return f; };
  for (const [name, n, want] of [['one.json', 1, 'It has 1 workout.'], ['two.json', 2, 'It has 2 workouts.']]) {
    dialogs.length = 0;
    await page.setInputFiles('#importFile', file(name, n)); await page.waitForTimeout(300);
    ok(dialogs.length === 1 && dialogs[0].includes(want), `restore confirm: "${want}"`, dialogs);
    await page.evaluate(() => { const i = document.querySelector('#importFile'); if (i) i.value = ''; });
  }
  ok(await page.evaluate(() => JSON.parse(localStorage.getItem('liftlog.v1')).sessions.length) === 2, 'dismissed restore changes nothing');
  fs.rmSync(dir, { recursive: true, force: true });
  ok(errors.length === 0, 'no page errors', errors);
  console.log(`\n${pass} passed, ${fail} failed`);
  await browser.close(); server.close(); process.exit(fail ? 1 : 0);
})();

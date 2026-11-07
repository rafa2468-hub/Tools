// Rep targets (1.0.7): suggested ranges filled into blank Reps once, the range in the Reps header while logging,
// "Change target" mid-workout (saved to the plan), and the double-progression "go heavier" nudge.
// The seed matches Maya's 11-07 screenshot: Reps left blank, a workout in progress ("Goal 2 sets · First time").
// Usage: node targets.js <app folder> <chrome.exe>
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
const OHP = 'd-barbell-overhead-press', LAT = 'd-dumbbell-lateral-raise', LP = 'd-leg-press', MINE = 'c-mine', FACE = 'd-face-pull', now = Date.now();
const empty = () => ({ w: '', r: '', done: false });
const seed = {
  v: 1, unit: 'lb', activeSplitId: 'sp', lastBackup: now, hideInstall: true, sessions: [],
  exercises: [{ id: MINE, name: 'My Move', group: 'Push', custom: true }],
  splits: [{ id: 'sp', name: 'PPL', days: [
    { id: 'push', name: 'Push', items: [{ exId: OHP, sets: 2, reps: '' }, { exId: LAT, sets: 2, reps: '' }, { exId: MINE, sets: 2, reps: '  ' }] },
    { id: 'legs', name: 'Legs', items: [{ exId: LP, sets: 3, reps: '' }, { exId: 'd-leg-extension', sets: 3, reps: '5' }] }] }],
  active: { id: 'a', dayId: 'push', splitId: 'sp', name: 'Push', start: now - 60e3, unit: 'lb', notes: '', entries: [
    { exId: OHP, name: 'Barbell Overhead Press', target: { sets: 2, reps: '' }, rest: null, sets: [empty(), empty()] },
    { exId: LAT, name: 'Dumbbell Lateral Raise', target: { sets: 2, reps: '' }, rest: null, sets: [empty(), empty()] },
    { exId: FACE, name: 'Face Pull', target: null, rest: null, sets: [empty(), empty(), empty()] }] }
};
(async () => {
  await new Promise(r => server.listen(0, '127.0.0.1', r));
  const URL = `http://127.0.0.1:${server.address().port}/app/`;
  const browser = await chromium.launch({ executablePath: CHROME, headless: true });
  const ctx = await browser.newContext({ viewport: { width: 390, height: 844 }, isMobile: true, hasTouch: true });
  const page = await ctx.newPage(); const errors = [];
  page.on('pageerror', x => errors.push(String(x)));
  page.on('dialog', d => d.accept());
  await page.goto(URL);
  await page.evaluate(s => localStorage.setItem('liftlog.v1', JSON.stringify(s)), seed);
  await page.reload(); await page.waitForTimeout(400);
  const S = () => page.evaluate(() => JSON.parse(JSON.stringify(S)));
  const card = i => page.locator('main .card').filter({ has: page.locator('[data-a="ex-menu"]') }).nth(i);
  const toastText = () => page.locator('#toast').innerText();

  // 1. Tier lists only name built-in exercises
  const tiers = await page.evaluate(() => { const all = Object.values(DEFAULTS).flat(); return { big: REPS_BIG.length, iso: REPS_ISO.length, bad: [...REPS_BIG, ...REPS_ISO].filter(n => !all.includes(n)), both: REPS_BIG.filter(n => REPS_ISO.includes(n)) }; });
  ok(tiers.bad.length === 0 && tiers.both.length === 0, `tier lists name only built-ins, no overlap (${tiers.big} big, ${tiers.iso} isolation/core)`, tiers);

  // 2. One-time fill on upgrade
  let s = await S();
  const items = s.splits[0].days.flatMap(d => d.items.map(it => it.exId + '=' + it.reps));
  ok(items.join() === [OHP + '=6-10', LAT + '=12-15', MINE + '=8-12', LP + '=8-12', 'd-leg-extension=5'].join(), 'blank plan reps filled by tier, typed "5" kept', items);
  ok(s.active.entries[0].target.reps === '6-10' && s.active.entries[1].target.reps === '12-15', 'workout in progress: blank targets filled', s.active.entries.map(e => e.target));
  ok(JSON.stringify(s.active.entries[2].target) === JSON.stringify({ sets: 3, reps: '12-15' }), 'workout in progress: added exercise gets a target', s.active.entries[2].target);
  ok(s.repsV === 1, 'repsV set');

  // 3. Logging screen shows the range
  let t = await card(0).innerText();
  ok(/Goal 2 × 6-10 ·/.test(t), 'goal line "Goal 2 × 6-10"', t.split('\n').slice(0, 3));
  ok((await card(0).locator('th .rng').innerText()) === '6-10' && (await card(1).locator('th .rng').innerText()) === '12-15', 'Reps header shows 6-10 and 12-15');
  ok((await card(0).locator('input[data-in="r"]').first().getAttribute('placeholder')) === '6', 'first-time reps hint is the bottom of the range');

  // 4. Change target mid-workout, saved to the plan
  const openTarget = async i => { await card(i).locator('[data-a="ex-menu"]').click(); await page.locator('.menu button', { hasText: 'Change target' }).click(); await page.waitForTimeout(150); };
  await openTarget(0);
  let sh = await page.locator('#sheet').innerText();
  ok(/Suggested reps: 6-10\. Saved to your plan for next time too\./.test(sh) && (await page.inputValue('#tgSets')) === '2' && (await page.inputValue('#tgReps')) === '6-10', 'target sheet: current values, suggestion, plan note', sh);
  await page.fill('#tgSets', '0'); await page.locator('[data-a="target-save"]').click(); await page.waitForTimeout(100);
  ok((await page.locator('#sheet.show').count()) === 1 && /1 to 20/.test(await toastText()), 'sets 0 refused, sheet stays open');
  await page.fill('#tgSets', '3'); await page.fill('#tgReps', '8-10'); await page.locator('#tgReps').press('Enter'); await page.waitForTimeout(200);
  s = await S();
  ok(JSON.stringify(s.active.entries[0].target) === JSON.stringify({ sets: 3, reps: '8-10' }) && s.active.entries[0].sets.length === 3, 'Enter saves: workout target 3 × 8-10, third set row added', s.active.entries[0]);
  ok(s.splits[0].days[0].items[0].sets === 3 && s.splits[0].days[0].items[0].reps === '8-10', 'plan item updated to 3 × 8-10', s.splits[0].days[0].items[0]);
  ok(/saved to your plan/.test(await toastText()) && (await card(0).locator('th .rng').innerText()) === '8-10', 'toast and header updated');
  // shrink: only empty, unticked rows come off the end
  await card(0).locator('input[data-in="r"]').nth(1).fill('7');
  await openTarget(0); await page.fill('#tgSets', '1'); await page.locator('[data-a="target-save"]').click(); await page.waitForTimeout(200);
  s = await S();
  ok(s.active.entries[0].sets.length === 2 && s.active.entries[0].sets[1].r === '7', 'shrink to 1 removes the empty third row, keeps the typed second', s.active.entries[0].sets);
  // exercise not on the plan day: this workout only
  const planBefore = JSON.stringify(s.splits);
  await openTarget(2);
  sh = await page.locator('#sheet').innerText();
  ok(/For this workout only/.test(sh), 'off-plan exercise: sheet says this workout only', sh);
  await page.fill('#tgReps', '15-20'); await page.locator('[data-a="target-save"]').click(); await page.waitForTimeout(200);
  s = await S();
  ok(s.active.entries[2].target.reps === '15-20' && JSON.stringify(s.splits) === planBefore && /this workout/.test(await toastText()), 'off-plan target saved to the workout only');

  // 5. Double-progression nudge while logging (lateral raise 2 × 12-15)
  const setRow = async (i, k, w, r, tick = true) => { const c = card(i); await c.locator('input[data-in="w"]').nth(k).fill(w); await c.locator('input[data-in="r"]').nth(k).fill(r); if (tick) await c.locator('[data-a="set-done"]').nth(k).click(); await page.waitForTimeout(120); };
  await setRow(1, 0, '10', '15');
  ok((await card(1).locator('.nudge').count()) === 0, 'one of two sets at the top: no nudge yet');
  await setRow(1, 1, '10', '15');
  ok((await card(1).locator('.nudge').innerText()) === 'Top of 12-15 on every set. Go heavier next time.', 'both sets at 15: nudge shows');
  await setRow(0, 0, '65', '10'); await setRow(0, 1, '65', '7');
  ok((await card(0).locator('.nudge').count()) === 0, 'OHP below the top (10 and 7 of 8-10): no nudge');
  await setRow(2, 0, '', '20'); await setRow(2, 1, '', '20'); await setRow(2, 2, '', '20');
  ok((await card(2).locator('.nudge').count()) === 0, 'bodyweight sets at the top: no "go heavier" nudge');

  // 6. Finish: the session remembers who hit the top
  await page.locator('main [data-a="finish"]').click(); await page.waitForTimeout(300);
  s = await S();
  const ent = id => s.sessions[0].entries.find(e => e.exId === id);
  ok(ent(LAT).hitTop === true && !ent(OHP).hitTop && !ent(FACE).hitTop, 'saved: lateral raise hitTop, OHP and bodyweight face pull not', s.sessions[0].entries.map(e => [e.exId, e.hitTop]));

  // 7. Next workout: "go heavier today", gone after the first tick
  await page.locator('nav.tabs [data-tab="today"]').click(); await page.waitForTimeout(150);
  await page.locator('[data-a="start-day"][data-day="push"]').click(); await page.waitForTimeout(250);
  ok((await card(1).locator('.nudge').innerText()) === 'Last time you hit the top of the range on every set. Go heavier today.', 'next workout: lateral raise says go heavier today');
  ok((await card(0).locator('.nudge').count()) === 0, 'next workout: OHP has no nudge');
  s = await S();
  ok(JSON.stringify(s.active.entries[0].target) === JSON.stringify({ sets: 1, reps: '8-10' }), 'next workout uses the changed plan target', s.active.entries[0].target);
  await setRow(1, 0, '12', '12');
  ok((await card(1).locator('.nudge').count()) === 0, 'nudge gone after the first tick');
  await page.locator('main [data-a="discard"]').click(); await page.waitForTimeout(200);

  // 8. Plan: new picks get the suggestion, the line shows, clearing sticks across reloads
  await page.locator('nav.tabs [data-tab="plan"]').click(); await page.waitForTimeout(150);
  await page.locator('.item[data-a="edit-day"][data-day="legs"]').click(); await page.waitForTimeout(150);
  await page.locator('[data-a="day-add-ex"]').click(); await page.fill('#pickSearch', 'Romanian'); await page.waitForTimeout(100);
  await page.locator('.pick', { hasText: 'Dumbbell Romanian Deadlift' }).click(); await page.locator('[data-a="pick-done"]').click(); await page.waitForTimeout(200);
  s = await S();
  ok(s.splits[0].days[1].items[2].reps === '8-12', 'new pick: Dumbbell Romanian Deadlift gets 8-12', s.splits[0].days[1].items[2]);
  const lines = await page.locator('main .item').allInnerTexts();
  ok(/Suggested reps: 8-12/.test(lines[0]) && /Suggested reps: 12-15/.test(lines[1]), 'Plan rows show "Suggested reps"', lines.map(l => l.replace(/\s+/g, ' ')));
  ok((await page.locator('input[data-in="item-reps"]').first().getAttribute('placeholder')) === null, 'Reps box has no look-alike placeholder');
  await page.locator('input[data-in="item-reps"]').first().fill('');
  await page.reload(); await page.waitForTimeout(400);
  s = await S();
  ok(s.splits[0].days[1].items[0].reps === '', 'cleared Reps stays cleared after a reload', s.splits[0].days[1].items[0]);

  // 9. A fresh install doesn't need the fill; old backups do
  ok(await page.evaluate(() => freshState().repsV === 1), 'fresh state starts with repsV 1');
  ok(await page.evaluate(s => normalize(JSON.parse(JSON.stringify(s))).splits[0].days[0].items[0].reps === '6-10', seed), 'restoring an old backup fills its blanks');

  ok(errors.length === 0, 'no page errors', errors);
  console.log(`\n${pass} passed, ${fail} failed`);
  await browser.close(); server.close(); process.exit(fail ? 1 : 0);
})();

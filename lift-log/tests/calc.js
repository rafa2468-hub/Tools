// Bar weight calculator (1.0.8): opens from a set's weight box, adds bar + plates (doubled, or one side) + anything
// else, fills that set without ticking it, and remembers the load per exercise.
// Usage: node calc.js <app folder> <chrome.exe> [screenshot folder]
const http = require('http'), fs = require('fs'), path = require('path');
const { chromium } = require('playwright-core');
const APP = path.resolve(process.argv[2]), CHROME = process.argv[3], SHOTS = process.argv[4];
const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.png': 'image/png', '.webmanifest': 'application/manifest+json' };
let pass = 0, fail = 0;
const ok = (c, n, x) => { if (c) { pass++; console.log('PASS', n); } else { fail++; console.log('FAIL', n, JSON.stringify(x === undefined ? '' : x)); } };
const server = http.createServer((req, res) => {
  let p = decodeURIComponent(req.url.split('?')[0]).replace(/^\/app\//, '/'); if (p.endsWith('/')) p += 'index.html';
  const f = path.join(APP, p);
  if (!f.startsWith(APP) || !fs.existsSync(f)) { res.writeHead(404); res.end(); return; }
  res.writeHead(200, { 'Content-Type': TYPES[path.extname(f)] || 'application/octet-stream' }); fs.createReadStream(f).pipe(res);
});
const SQ = 'd-barbell-back-squat', TB = 'd-t-bar-row', LP = 'd-leg-press', MINE = 'c-mine', now = Date.now();
const empty = () => ({ w: '', r: '', done: false });
// A 1.0.7-shaped save: no calc field.
const seed = unit => ({
  v: 1, unit, activeSplitId: 'sp', lastBackup: now, hideInstall: true, repsV: 1, sessions: [],
  exercises: [{ id: MINE, name: 'My Move', group: 'Push', custom: true }],
  splits: [{ id: 'sp', name: 'PPL', days: [{ id: 'legs', name: 'Legs', items: [{ exId: SQ, sets: 3, reps: '6-10' }, { exId: TB, sets: 2, reps: '6-10' }, { exId: LP, sets: 2, reps: '8-12' }] }] }],
  active: { id: 'a', dayId: 'legs', splitId: 'sp', name: 'Legs', start: now - 60e3, unit, notes: '', entries: [
    { exId: SQ, name: 'Barbell Back Squat', target: { sets: 3, reps: '6-10' }, rest: null, sets: [empty(), empty(), empty()] },
    { exId: TB, name: 'T-Bar Row', target: { sets: 2, reps: '6-10' }, rest: null, sets: [empty(), empty()] },
    { exId: LP, name: 'Leg Press', target: { sets: 2, reps: '8-12' }, rest: null, sets: [empty(), empty()] }] }
});
(async () => {
  await new Promise(r => server.listen(0, '127.0.0.1', r));
  const URL = `http://127.0.0.1:${server.address().port}/app/`;
  const browser = await chromium.launch({ executablePath: CHROME, headless: true });
  const shoot = async (p, name) => { if (SHOTS) { fs.mkdirSync(SHOTS, { recursive: true }); await p.screenshot({ path: path.join(SHOTS, name + '.png') }); } };
  const open = async (scheme, s) => {
    const ctx = await browser.newContext({ viewport: { width: 390, height: 844 }, isMobile: true, hasTouch: true, colorScheme: scheme });
    const page = await ctx.newPage(); page.errors = [];
    page.on('pageerror', x => page.errors.push(String(x)));
    page.on('dialog', d => d.accept());
    await page.goto(URL);
    await page.evaluate(x => localStorage.setItem('liftlog.v1', JSON.stringify(x)), s);
    await page.reload(); await page.waitForTimeout(400);
    return page;
  };
  const page = await open('light', seed('lb'));
  const S = () => page.evaluate(() => JSON.parse(JSON.stringify(S)));
  const stored = () => page.evaluate(() => JSON.parse(localStorage.getItem('liftlog.v1')));
  const card = i => page.locator('main .card').filter({ has: page.locator('[data-a="ex-menu"]') }).nth(i);
  const calc = async (i, k) => { await card(i).locator('[data-a="calc-open"]').nth(k).click(); await page.waitForTimeout(120); };
  const plate = async (p, n = 1) => { for (let x = 0; x < n; x++) await page.locator(`.plate[data-p="${p}"]`).click(); };
  const chip = t => page.locator('#sheet .chip', { hasText: new RegExp('^' + t + '$') }).click();
  const total = () => page.locator('#calcTotal').innerText();
  const brk = () => page.locator('#calcBreak').innerText();
  const useBtn = page.locator('#calcUse');
  const use = async () => { await useBtn.click(); await page.waitForTimeout(150); };
  const wBox = (i, k) => card(i).locator('input[data-in="w"]').nth(k);

  // 1. Old save upgrades cleanly; every unticked set has the button, inside the weight box
  let s = await S();
  ok(JSON.stringify(s.calc) === '{}', 'a 1.0.7 save gets an empty calc', s.calc);
  ok((await page.locator('[data-a="calc-open"]').count()) === 7, 'one calculator button per unticked set (7)');
  const geo = await page.evaluate(() => { const b = document.querySelector('.calcbtn').getBoundingClientRect(), i = document.querySelector('input[data-in="w"]').getBoundingClientRect(); return { b: [b.left, b.right, b.width, b.height], i: [i.left, i.right, i.height], over: document.documentElement.scrollWidth > innerWidth }; });
  ok(geo.b[0] >= geo.i[0] && geo.b[1] <= geo.i[1] + 0.5 && geo.b[2] >= 36 && geo.b[3] >= 40, 'button sits inside the weight box and is tappable', geo);
  ok(!geo.over, 'no sideways scroll on the logging screen', geo);
  await shoot(page, 'logging-light');

  // 2. Opening: starts at the standard bar
  await calc(0, 0);
  ok((await page.locator('#sheet.show').count()) === 1 && /Barbell Back Squat, set 1/.test(await page.locator('#sheet').innerText()), 'sheet opens, names the exercise and set');
  ok(/^45\s+lb$/.test(await total()), 'starts at the 45 lb bar', await total());
  ok(await page.locator('#calcUndo').isDisabled() && await page.locator('#calcClear').isDisabled(), 'Undo and Clear off with nothing loaded');
  ok((await page.locator('.plate').allInnerTexts()).join() === '45,35,25,10,5,2.5', 'lb plates: 45, 35, 25, 10, 5, 2.5');
  ok((await page.locator('#calcBars .chip').allInnerTexts()).join() === 'None,15,35,45', 'lb bars: None, 15, 35, 45');

  // 3. The arithmetic: 2 x 45 + 25 + 2.5 per side on a 45 bar = 45 + 2 x 117.5 = 280
  await plate(45, 2); await plate(25); await plate(2.5);
  ok(/^280\s+lb$/.test(await total()), '45 bar + 45,45,25,2.5 per side = 280', await total());
  ok((await brk()) === 'Bar 45 + 117.5 per side', 'breakdown line', await brk());
  ok((await page.locator('.plate[data-p="45"] .ct').innerText()) === '×2', 'plate shows its count');
  await shoot(page, 'sheet-light');
  await page.locator('#calcUndo').click();
  ok(/^275\s+lb$/.test(await total()), 'Undo takes off the last plate (2.5 per side)', await total());
  await chip('35');
  ok(/^265\s+lb$/.test(await total()) && (await brk()) === 'Bar 35 + 115 per side', 'switching to a 35 bar', await total());
  await chip('None');
  ok(/^230\s+lb$/.test(await total()) && (await brk()) === '115 per side', 'no bar', await brk());
  await chip('45');

  // 4. One side
  await chip('One side');
  ok(/^160\s+lb$/.test(await total()) && (await brk()) === 'Bar 45 + 115 on one side', 'one side counts plates once', await brk());
  ok((await page.locator('#calcPlateHead').innerText()) === 'Plates on the loaded side', 'heading follows the side switch');
  await chip('Both sides');
  ok(/^275\s+lb$/.test(await total()), 'back to both sides');

  // 5. Anything else: counted once, decimals and commas, junk ignored
  await page.fill('#calcOther', '12.5');
  ok(/^287\.5\s+lb$/.test(await total()) && (await brk()) === 'Bar 45 + 115 per side + 12.5 other', 'other counted once', await brk());
  await page.fill('#calcOther', '7,5');
  ok(/^282\.5\s+lb$/.test(await total()), 'comma decimal accepted', await total());
  await page.fill('#calcOther', 'abc');
  ok(/^275\s+lb$/.test(await total()), 'junk in the box adds nothing', await total());
  await page.fill('#calcOther', '10');
  await page.locator('#calcOther').press('Enter'); await page.waitForTimeout(80);
  ok((await page.locator('#sheet.show').count()) === 1 && /^285\s+lb$/.test(await total()), 'Enter closes the keyboard, not the sheet');
  // keep typing focus across a live redraw
  await page.locator('#calcOther').focus(); await page.keyboard.type('5');
  ok((await page.inputValue('#calcOther')) === '105' && (await page.evaluate(() => document.activeElement.id)) === 'calcOther', 'box keeps focus while the total redraws', await page.inputValue('#calcOther'));
  await page.fill('#calcOther', '10');

  // 6. Use: fills only this set, doesn't tick, doesn't start the rest timer, saved
  await use();
  ok((await page.locator('#sheet.show').count()) === 0, 'Use closes the sheet');
  ok((await wBox(0, 0).inputValue()) === '285', 'set 1 weight box reads 285', await wBox(0, 0).inputValue());
  ok((await wBox(0, 1).inputValue()) === '' && (await wBox(0, 2).inputValue()) === '', 'other sets left alone');
  let d = await stored();
  ok(d.active.entries[0].sets[0].w === '285' && d.active.entries[0].sets[0].done === false, 'saved to the phone, not ticked', d.active.entries[0].sets[0]);
  ok(!d.active.rest && (await page.locator('#restbar.show').count()) === 0, 'rest timer not started');
  ok(JSON.stringify(d.calc[SQ]) === JSON.stringify({ unit: 'lb', bar: 45, plates: [45, 45, 25], other: '10', one: false }), 'load remembered for the exercise', d.calc[SQ]);

  // 7. Next set starts from that load; tap on 5s and use it
  await calc(0, 1);
  ok(/^285\s+lb$/.test(await total()) && (await page.inputValue('#calcOther')) === '10', 'next set reopens with the same load', await total());
  await plate(5);
  ok(/^295\s+lb$/.test(await total()), 'add a 5 per side = 295', await total());
  await use();
  ok((await wBox(0, 1).inputValue()) === '295' && (await wBox(0, 0).inputValue()) === '285', 'set 2 is 295, set 1 still 285');

  // 8. Cancel throws away changes
  await calc(0, 2); await plate(45);
  await page.locator('#sheet [data-a="sheet-close"]').click(); await page.waitForTimeout(100);
  d = await stored();
  ok((await wBox(0, 2).inputValue()) === '' && d.calc[SQ].plates.length === 4, 'Cancel: box empty, remembered load unchanged', d.calc[SQ]);

  // 9. Clear; a zero total can't be used; bar stays
  await calc(1, 0); await plate(25); await page.locator('#calcClear').click();
  ok(/^45\s+lb$/.test(await total()) && (await page.locator('.plate.on').count()) === 0, 'Clear takes off plates, leaves the bar');
  await chip('None');
  ok(/^0\s+lb$/.test(await total()) && await useBtn.isDisabled(), 'zero total: Use is off');
  // One side remembered per exercise (T-bar), independent of the squat
  await plate(45, 2); await chip('One side');
  ok(/^90\s+lb$/.test(await total()), 'T-bar row: 2 x 45 on one side, no bar = 90', await total());
  await use();
  await calc(1, 1);
  ok(/^90\s+lb$/.test(await total()) && (await page.locator('#calcPlateHead').innerText()) === 'Plates on the loaded side', 'one side and no bar remembered on the T-bar');
  await page.locator('#sheet [data-a="sheet-close"]').click(); await page.waitForTimeout(100);
  await calc(2, 0);
  ok(/^45\s+lb$/.test(await total()), 'an exercise with no history starts fresh', await total());
  await chip('None');
  await page.fill('#calcOther', '118'); await plate(45, 3);
  ok(/^388\s+lb$/.test(await total()), 'leg press: 118 sled + 3 x 45 per side = 388', await total());
  await use();

  // 10. The filled weight behaves like a typed one
  await card(0).locator('input[data-in="r"]').first().fill('8');
  await card(0).locator('[data-a="set-done"]').first().click(); await page.waitForTimeout(150);
  d = await stored();
  ok(d.active.entries[0].sets[0].done && d.active.entries[0].sets[0].w === '285', 'ticking a calculated set logs it');
  ok((await page.locator('#restbar.show').count()) === 1, 'the tick starts the rest timer as usual');
  ok((await card(0).locator('tr.done [data-a="calc-open"]').count()) === 0 && (await card(0).locator('[data-a="calc-open"]').count()) === 2, 'ticked set loses its button');
  ok((await wBox(0, 0).inputValue()) === '285', 'a ticked set keeps its value');

  // 11. Survives closing the app; goes into a finished workout
  await page.reload(); await page.waitForTimeout(400);
  await calc(0, 1); ok(/^295\s+lb$/.test(await total()), 'remembered load survives closing the app', await total());
  await page.locator('#sheet [data-a="sheet-close"]').click(); await page.waitForTimeout(100);
  await page.locator('main [data-a="finish"]').click(); await page.waitForTimeout(300);
  s = await S();
  ok(s.sessions[0].entries[0].sets[0].w === 285 && s.sessions[0].entries[0].sets[0].r === 8, 'finished workout holds 285 × 8', s.sessions[0].entries[0].sets);

  // 12. Next workout: last week's load comes back
  await page.locator('nav.tabs [data-tab="today"]').click(); await page.waitForTimeout(150);
  await page.locator('[data-a="start-day"][data-day="legs"]').click(); await page.waitForTimeout(250);
  await calc(0, 0); ok(/^295\s+lb$/.test(await total()), 'next workout starts from the last load used', await total());
  await page.locator('#sheet [data-a="sheet-close"]').click(); await page.waitForTimeout(100);

  // 13. Data handling
  ok(await page.evaluate(() => { const c = freshState().calc; return !!c && !Object.keys(c).length; }), 'fresh state has an empty calc');
  ok(await page.evaluate(() => { const d = freshState(); d.calc = null; const a = normalize(d).calc; const e = freshState(); e.calc = [1]; const b = normalize(e).calc; return JSON.stringify([a, b]) === '[{},{}]'; }), 'a bad calc in a backup is reset');
  ok(await page.evaluate(id => { S.calc[id] = { unit: 'lb', bar: 999, plates: [45, 7, 'x'], other: 5, one: 1 }; const c = calcFor(id, 'lb'); delete S.calc[id]; return c.bar === 45 && c.plates.join() === '45' && c.other === '' && c.one === true; }, 'd-front-squat'), 'a damaged saved load is cleaned on open');
  const bak = await page.evaluate(() => JSON.stringify(normalize(JSON.parse(JSON.stringify(S))).calc));
  ok(JSON.parse(bak)[SQ] && JSON.parse(bak)[TB], 'the load survives a backup and restore', bak);
  await page.evaluate(id => { S.calc[id] = { unit: 'lb', bar: 45, plates: [], other: '', one: false }; S.exercises.find(e => e.id === id) || S.exercises.push({ id, name: 'My Move', group: 'Push', custom: true }); ui.tab = 'exercises'; ui.viewEx = id; A['delete-ex'](); }, MINE);
  ok(!(await S()).calc[MINE], 'deleting her own exercise removes its saved load');
  ok(page.errors.length === 0, 'no page errors (lb run)', page.errors);

  // 14. Kilos
  const kg = await open('dark', seed('kg'));
  await kg.locator('[data-a="calc-open"]').first().click(); await kg.waitForTimeout(120);
  ok(/^20\s+kg$/.test(await kg.locator('#calcTotal').innerText()), 'kg: starts at the 20 kg bar');
  ok((await kg.locator('.plate').allInnerTexts()).join() === '25,20,15,10,5,2.5,1.25', 'kg plates');
  ok((await kg.locator('#calcBars .chip').allInnerTexts()).join() === 'None,10,15,20', 'kg bars');
  for (const p of [20, 10, 2.5, 1.25]) await kg.locator(`.plate[data-p="${p}"]`).click();
  ok(/^87\.5\s+kg$/.test(await kg.locator('#calcTotal').innerText()), '20 bar + 20,10,2.5,1.25 per side = 87.5', await kg.locator('#calcTotal').innerText());
  await shoot(kg, 'sheet-kg-dark');
  await kg.locator('#calcUse').click(); await kg.waitForTimeout(150);
  ok((await kg.locator('input[data-in="w"]').first().inputValue()) === '87.5', 'kg: fills 87.5');
  await shoot(kg, 'logging-dark');
  // A remembered lb load doesn't leak into a kg workout
  ok(await kg.evaluate(id => { S.calc[id] = { unit: 'lb', bar: 45, plates: [45], other: '', one: false }; const c = calcFor(id, 'kg'); return c.bar === 20 && !c.plates.length; }, TB), 'a load saved in lb starts fresh in kg');
  ok(kg.errors.length === 0, 'no page errors (kg run)', kg.errors);

  // 15. Small phone: the sheet fits and scrolls
  const small = await browser.newContext({ viewport: { width: 320, height: 568 }, isMobile: true, hasTouch: true });
  const sp = await small.newPage();
  await sp.goto(URL); await sp.evaluate(x => localStorage.setItem('liftlog.v1', JSON.stringify(x)), seed('kg')); await sp.reload(); await sp.waitForTimeout(400);
  await sp.locator('[data-a="calc-open"]').first().click(); await sp.waitForTimeout(150);
  const fit = await sp.evaluate(() => { const u = document.querySelector('#calcUse').getBoundingClientRect(), h = document.querySelector('.sh-head').getBoundingClientRect(); return { useBottom: u.bottom, headTop: h.top, H: innerHeight, over: document.documentElement.scrollWidth > innerWidth }; });
  ok(fit.useBottom <= fit.H && fit.headTop >= 0 && !fit.over, '320 × 568: Use button and header on screen', fit);
  await shoot(sp, 'sheet-small');

  console.log(`\n${pass} passed, ${fail} failed`);
  await browser.close(); server.close(); process.exit(fail ? 1 : 0);
})();

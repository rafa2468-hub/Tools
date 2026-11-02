// Rest timer (1.0.3): countdown from the exercise's or default rest, −15/+15/Skip, beep once at zero,
// kept through a reload and tab switches, screen kept awake only while resting, time between sets
// saved per set and shown on the workout page, Settings default and sound switch, and a 1.0.2-style
// workout in progress still working. Sound and wake lock are stubbed (headless Chrome has neither
// a speaker nor a screen), so this checks what the app asks for, not what an iPhone does with it.
// Usage: node rest.js <app folder> <chrome.exe>
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

const BENCH = 'd-barbell-bench-press', FLY = 'd-cable-fly', DAY = 864e5, now = Date.now();
const seed = {
  v: 1, unit: 'lb', exercises: [], activeSplitId: 'sp', active: null, lastBackup: now, hideInstall: true,
  splits: [{ id: 'sp', name: 'PPL', days: [{ id: 'push', name: 'Push', items: [{ exId: BENCH, sets: 3, reps: '8' }, { exId: FLY, sets: 3, reps: '12' }] }] }],
  sessions: [{ id: 'old', dayId: 'push', splitId: 'sp', name: 'Push', start: now - 7 * DAY, end: now - 7 * DAY + 3600e3, unit: 'lb', notes: '', entries: [{ exId: BENCH, name: 'Barbell Bench Press', sets: [{ w: 95, r: 8 }] }] }]
};

function stubs() {
  window.__t = { ctx: 0, osc: 0, resumes: 0, suspends: 0, wake: 0, released: 0 };
  class FakeAC {
    constructor() { __t.ctx++; this.state = 'suspended'; this.currentTime = 0; this.destination = {}; }
    resume() { __t.resumes++; this.state = 'running'; return Promise.resolve(); }
    suspend() { __t.suspends++; this.state = 'suspended'; return Promise.resolve(); }
    createOscillator() { __t.osc++; return { type: '', frequency: { value: 0 }, connect() { }, start() { }, stop() { } }; }
    createGain() { return { gain: { setValueAtTime() { }, exponentialRampToValueAtTime() { } }, connect() { } }; }
  }
  window.AudioContext = FakeAC; window.webkitAudioContext = FakeAC;
  Object.defineProperty(navigator, 'audioSession', { value: { type: 'auto' }, configurable: true });
  Object.defineProperty(navigator, 'wakeLock', { configurable: true, value: { request() {
    __t.wake++; const l = { h: [], addEventListener(e, f) { this.h.push(f); }, release() { __t.released++; this.h.forEach(f => f()); return Promise.resolve(); } };
    return Promise.resolve(l);
  } } });
}

(async () => {
  await new Promise(r => server.listen(0, '127.0.0.1', r));
  const URL = `http://127.0.0.1:${server.address().port}/app/`;
  const browser = await chromium.launch({ executablePath: CHROME, headless: true });
  const run = async (scheme) => {
    const ctx = await browser.newContext({ viewport: { width: 390, height: 844 }, deviceScaleFactor: 2, isMobile: true, hasTouch: true, colorScheme: scheme,
      userAgent: 'Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Mobile/15E148 Safari/604.1' });
    await ctx.addInitScript(stubs);
    const page = await ctx.newPage(); const errors = [];
    page.on('pageerror', x => errors.push(String(x))); page.on('console', m => { if (m.type() === 'error') errors.push(m.text()); });
    page.on('dialog', d => d.accept());
    return { ctx, page, errors };
  };
  const { page, errors } = await run('light');
  const T = () => page.evaluate(() => ({ ...__t }));
  const st = () => page.evaluate(() => JSON.parse(localStorage.getItem('liftlog.v1')));
  const barText = () => page.locator('#restText').innerText();
  const barShown = async () => (await page.locator('#restbar.show').count()) === 1;
  const barGo = async () => (await page.locator('#restbar.go').count()) === 1;
  const secsOf = t => { const m = t.match(/(\d+):(\d\d)/); return m ? +m[1] * 60 + +m[2] : NaN; };
  const tick = (e, s) => page.locator(`[data-a="set-done"][data-e="${e}"][data-s="${s}"]`).click();
  const reps = (e, s, r) => page.fill(`[data-in="r"][data-e="${e}"][data-s="${s}"]`, String(r));
  const tab = t => page.locator(`nav.tabs [data-tab="${t}"]`).click();
  const shift = ms => page.evaluate(ms => { S.active.rest.start -= ms; save(); renderRest(); }, ms);

  await page.goto(URL);
  await page.evaluate(s => localStorage.setItem('liftlog.v1', JSON.stringify(s)), seed);
  await page.reload();

  // 1. Plan: each exercise gets a Rest choice, default first
  await tab('plan'); await page.locator('.item[data-a="edit-day"]').first().click();
  const sel = page.locator('select[data-in="item-rest"]').first();
  ok((await sel.locator('option:checked').innerText()) === 'Default (1:30)', 'plan rest shows Default (1:30)', await sel.locator('option:checked').innerText());
  await sel.selectOption('120');
  ok((await st()).splits[0].days[0].items[0].rest === 120, 'picking 2:00 for bench is saved');
  await page.screenshot({ path: path.join(SHOTS, 'rest-plan-light.png') });

  // 2. Settings: default rest and sound
  await tab('today'); await page.locator('[data-a="open-settings"]').click();
  await page.locator('[data-a="rest-default"][data-n="60"]').click();
  ok((await st()).restDefault === 60, 'Settings default 1:00 saved');
  await page.screenshot({ path: path.join(SHOTS, 'rest-settings-light.png'), fullPage: true });
  await tab('plan'); await page.locator('.item[data-a="edit-day"]').first().click();
  ok((await page.locator('select[data-in="item-rest"]').nth(1).locator('option:checked').innerText()) === 'Default (1:00)', 'plan default label follows Settings');
  await tab('today'); await page.locator('[data-a="open-settings"]').click();
  await page.locator('[data-a="rest-default"][data-n="90"]').click();
  await page.locator('[data-a="rest-sound"][data-v="off"]').click();
  ok((await st()).restSound === false && (await page.locator('[data-a="rest-sound"][data-v="off"].on').count()) === 1, 'Settings sound off saved and shown');
  await page.locator('[data-a="rest-sound"][data-v="on"]').click();
  ok((await st()).restSound === true, 'Settings sound back on');
  await page.locator('[data-a="back-today"]').click();

  // 3. Start: the bench goal line names its rest; ticking starts the countdown
  await page.locator('[data-a="start-day"]').click();
  const benchHead = await page.locator('#view .card').first().locator('.small.muted').first().innerText();
  ok(benchHead.includes('Rest 2:00'), 'bench goal line shows Rest 2:00', benchHead);
  const flyHead = await page.locator('#view .card').nth(1).locator('.small.muted').first().innerText();
  ok(!flyHead.includes('Rest'), 'exercise on default rest shows no Rest', flyHead);
  ok(!(await barShown()), 'no rest bar before a set');
  await tick(0, 0);
  ok(await barShown() && !(await barGo()), 'ticking a set shows the countdown');
  let t = await barText(); ok(/^Rest 2:00$|^Rest 1:59$/.test(t), 'counts down from bench rest 2:00', t);
  let tt = await T();
  ok(tt.ctx === 1 && tt.resumes >= 1 && tt.wake === 1, 'sound unlocked and screen kept awake on the tap', tt);
  ok(await page.evaluate(() => navigator.audioSession.type) === 'transient', 'audio session set to transient (plays over music)');
  ok((await st()).active.rest.secs === 120, 'rest saved with the workout');
  await page.screenshot({ path: path.join(SHOTS, 'rest-countdown-light.png') });

  // 4. −15 / +15
  await page.locator('[data-a="rest-adj"][data-d="15"]').click();
  t = await barText(); ok(secsOf(t) >= 133 && secsOf(t) <= 135, '+15 adds 15 s', t);
  ok((await st()).active.entries[0].rest === 135, '+15 sticks for the rest of this workout');
  await page.locator('[data-a="rest-adj"][data-d="-15"]').click(); await page.locator('[data-a="rest-adj"][data-d="-15"]').click();
  t = await barText(); ok(secsOf(t) >= 103 && secsOf(t) <= 105, '−15 twice takes 30 s off', t);
  ok((await st()).splits[0].days[0].items[0].rest === 120, 'the plan keeps its own 2:00');

  // 5. Other tabs and closing the app
  await tab('history'); ok(await barShown(), 'rest bar shows on History');
  await tab('plan'); ok(await barShown(), 'rest bar shows on Plan');
  await tab('today');
  await page.reload();
  t = await barText(); ok(await barShown() && secsOf(t) >= 95 && secsOf(t) <= 105, 'countdown survives closing the app', t);

  // 6. Reaching zero: beep once, green, screen let go
  await reps(0, 1, 8); await tick(0, 1); // a tap unlocks sound again after the reload
  tt = await T(); const osc0 = tt.osc, rel0 = tt.released;
  const secs = (await st()).active.rest.secs;
  ok(secs === 105, 'next set uses the adjusted 1:45', secs);
  await shift(secs * 1000 - 1500);
  await page.waitForTimeout(2600);
  tt = await T();
  ok(await barGo() && /^Go! \+0:0\d$/.test(await barText()), 'at zero the bar turns green and counts over', await barText());
  ok(tt.osc - osc0 === 3, 'three beeps at zero', tt.osc - osc0);
  ok(tt.released > rel0, 'screen wake released once rest is over', tt);
  ok((await st()).active.rest.beeped === true, 'beep remembered');
  await page.screenshot({ path: path.join(SHOTS, 'rest-go-light.png') });
  await page.waitForTimeout(2200);
  ok((await T()).osc - osc0 === 3, 'beeps only once');
  ok((await T()).suspends >= 1, 'sound let go after the beep');
  await page.reload(); await page.waitForTimeout(1200);
  ok(await barGo() && (await T()).osc === 0, 'reopening after zero shows Go without beeping again');

  // 7. +15 after zero restarts a countdown; Skip hides it
  await page.locator('[data-a="rest-adj"][data-d="15"]').click();
  t = await barText(); ok(!(await barGo()) && secsOf(t) >= 14 && secsOf(t) <= 15, '+15 after zero gives 15 more seconds', t);
  await page.locator('[data-a="rest-skip"]').click();
  ok(!(await barShown()) && (await st()).active.rest === null, 'Skip hides the timer');

  // 8. Time between sets, untick
  let a = (await st()).active;
  ok(a.entries[0].sets[0].rest == null, 'first set of the workout has no time between sets');
  await page.evaluate(() => { for (const s of S.active.entries[0].sets) if (s.t) s.t -= 100000; save(); });
  await reps(0, 2, 7); await tick(0, 2);
  a = (await st()).active;
  ok(a.entries[0].sets[2].rest >= 100 && a.entries[0].sets[2].rest <= 115, 'time since the previous tick saved on the set', a.entries[0].sets[2].rest);
  await tick(0, 2);
  a = (await st()).active;
  ok(!a.entries[0].sets[2].done && a.entries[0].sets[2].rest === undefined && a.rest === null && !(await barShown()), 'unticking the set that started the timer stops it and clears its time');
  await tick(0, 2);
  a = (await st()).active;
  ok(a.entries[0].sets[2].rest >= 100 && a.entries[0].sets[2].rest <= 115, 'reticking measures from the previous set, not itself', a.entries[0].sets[2].rest);

  // 9. Way past zero: dropped; long gap not counted
  await shift((a.rest.secs + 16 * 60) * 1000); await page.waitForTimeout(1200);
  ok(!(await barShown()) && (await st()).active.rest === null, 'timer dropped 15 min after zero');
  await page.evaluate(() => { for (const en of S.active.entries) for (const s of en.sets) if (s.t) s.t -= 20 * 60000; save(); });
  await reps(1, 0, 12); await tick(1, 0);
  a = (await st()).active;
  ok(a.entries[1].sets[0].rest === null, 'a gap over 15 min is not counted as rest', a.entries[1].sets[0].rest);
  ok(a.rest.secs === 90, 'fly uses the 1:30 default', a.rest.secs);

  // 10. Sound off
  await page.evaluate(() => { S.restSound = false; save(); }); // Settings isn't reachable mid-workout; its switch is tested in step 2
  const osc1 = (await T()).osc;
  await shift(90 * 1000 - 1000); await page.waitForTimeout(2200);
  ok(await barGo() && (await T()).osc === osc1, 'no beep with sound off, still turns green');
  await page.evaluate(() => { S.restSound = true; save(); });

  // 11. Finish: saved per set, shown on the workout page; older workouts unchanged
  await page.locator('#right [data-a="finish"]').click();
  const s = await st();
  ok(s.active === null && !(await barShown()), 'finish clears the timer');
  const w = s.sessions.find(x => x.id !== 'old');
  ok(w.entries[0].sets[2].rest >= 100 && w.entries[0].sets[0].rest === undefined && !('t' in w.entries[0].sets[0]), 'saved sets keep time between sets, nothing else extra', w.entries[0].sets);
  const stats = await page.locator('#view .card.row').first().innerText();
  ok(/between sets/.test(stats), 'workout page shows time between sets', stats);
  ok((await page.locator('#view').innerText()).includes('after the last set'), 'per-set time shown');
  await page.screenshot({ path: path.join(SHOTS, 'rest-session-light.png'), fullPage: true });
  await page.locator('[data-a="back-history"]').click();
  await page.locator('.item[data-a="view-session"][data-id="old"]').click();
  ok(!/between sets/.test(await page.locator('#view .card.row').first().innerText()), 'older workout shows no rest stat');

  // 12. A workout started on 1.0.2 (no rest fields) keeps working
  const legacy = { ...seed, active: { id: 'a1', dayId: 'push', splitId: 'sp', name: 'Push', start: Date.now() - 600e3, unit: 'lb', notes: '',
    entries: [{ exId: BENCH, name: 'Barbell Bench Press', target: { sets: 3, reps: '8' }, sets: [{ w: '95', r: '8', done: true }, { w: '', r: '', done: false }] }] } };
  delete legacy.restDefault; delete legacy.restSound;
  await page.evaluate(x => localStorage.setItem('liftlog.v1', JSON.stringify(x)), legacy);
  await page.reload();
  ok(!(await barShown()), '1.0.2 workout opens without a timer');
  await reps(0, 1, 8); await tick(0, 1);
  a = (await st()).active;
  ok(await barShown() && a.rest.secs === 90 && a.entries[0].sets[1].rest === null && (await st()).restDefault === 90, '1.0.2 workout: tick starts 1:30, no between-sets time from the old untimed tick');

  ok(errors.length === 0, 'no page errors', errors);

  // Dark mode screenshots
  const d = await run('dark');
  await d.page.goto(URL);
  await d.page.evaluate(s => localStorage.setItem('liftlog.v1', JSON.stringify(s)), seed);
  await d.page.reload();
  await d.page.locator('[data-a="start-day"]').click();
  await d.page.fill('[data-in="r"][data-e="0"][data-s="0"]', '8'); await d.page.locator('[data-a="set-done"][data-e="0"][data-s="0"]').click();
  await d.page.screenshot({ path: path.join(SHOTS, 'rest-countdown-dark.png') });
  await d.page.evaluate(() => { S.active.rest.start -= 89000; save(); renderRest(); }); await d.page.waitForTimeout(2200);
  await d.page.screenshot({ path: path.join(SHOTS, 'rest-go-dark.png') });
  await d.page.locator('nav.tabs [data-tab="plan"]').click(); await d.page.locator('.item[data-a="edit-day"]').first().click();
  await d.page.screenshot({ path: path.join(SHOTS, 'rest-plan-dark.png') });
  ok(d.errors.length === 0, 'no page errors (dark)', d.errors);

  console.log(`\n${pass} passed, ${fail} failed`);
  await browser.close(); server.close(); process.exit(fail ? 1 : 0);
})().catch(e => { console.error(e); process.exit(1); });

// Rest-over flash (1.0.5): iPhone web audio follows the silent switch, so when rest hits zero the whole
// screen flashes green with "Go!" as well as beeping. Checks: hidden until zero, shows at zero with the beep,
// shows with the sound off too, taps go through it, gone after ~2.5 s, no flash on a late return or after
// Skip, a steady fade (no blinking) with reduced motion, and the Settings line about silent mode.
// Sound and wake lock are stubbed, as in rest.js. Usage: node flash.js <app folder> <chrome.exe>
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
  window.__t = { osc: 0 };
  class FakeAC {
    constructor() { this.state = 'suspended'; this.currentTime = 0; this.destination = {}; }
    resume() { this.state = 'running'; return Promise.resolve(); }
    suspend() { this.state = 'suspended'; return Promise.resolve(); }
    createOscillator() { __t.osc++; return { type: '', frequency: { value: 0 }, connect() { }, start() { }, stop() { } }; }
    createGain() { return { gain: { setValueAtTime() { }, exponentialRampToValueAtTime() { } }, connect() { } }; }
  }
  window.AudioContext = FakeAC; window.webkitAudioContext = FakeAC;
  Object.defineProperty(navigator, 'audioSession', { value: { type: 'auto' }, configurable: true });
  Object.defineProperty(navigator, 'wakeLock', { configurable: true, value: { request() {
    const l = { h: [], addEventListener(e, f) { this.h.push(f); }, release() { this.h.forEach(f => f()); return Promise.resolve(); } };
    return Promise.resolve(l);
  } } });
}

(async () => {
  await new Promise(r => server.listen(0, '127.0.0.1', r));
  const URL = `http://127.0.0.1:${server.address().port}/app/`;
  const browser = await chromium.launch({ executablePath: CHROME, headless: true });
  const run = async (scheme, reducedMotion = 'no-preference') => {
    const ctx = await browser.newContext({ viewport: { width: 390, height: 844 }, deviceScaleFactor: 2, isMobile: true, hasTouch: true, colorScheme: scheme, reducedMotion,
      userAgent: 'Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Mobile/15E148 Safari/604.1' });
    await ctx.addInitScript(stubs);
    const page = await ctx.newPage(); const errors = [];
    page.on('pageerror', x => errors.push(String(x))); page.on('console', m => { if (m.type() === 'error') errors.push(m.text()); });
    page.on('dialog', d => d.accept());
    await page.goto(URL);
    await page.evaluate(s => localStorage.setItem('liftlog.v1', JSON.stringify(s)), seed);
    await page.reload();
    return { page, errors };
  };
  const helpers = page => ({
    shown: async () => (await page.locator('#restFlash.show').count()) === 1,
    osc: () => page.evaluate(() => __t.osc),
    tick: async (e, s) => { await page.fill(`[data-in="r"][data-e="${e}"][data-s="${s}"]`, '8'); await page.locator(`[data-a="set-done"][data-e="${e}"][data-s="${s}"]`).click(); },
    toZero: lead => page.evaluate(lead => { const r = S.active.rest; r.start -= r.secs * 1000 - lead; save(); renderRest(); }, lead),
    waitShown: () => page.waitForSelector('#restFlash.show', { state: 'attached', timeout: 4000 }).then(() => true, () => false),
  });

  const { page, errors } = await run('light'); const h = helpers(page);

  // 1. Settings line, and the flash is there but hidden
  await page.locator('[data-a="open-settings"]').click();
  const settings = await page.locator('#view').innerText();
  ok(settings.includes('Silent mode mutes the beep, so the screen also flashes green when rest is over.'), 'Settings says silent mode mutes the beep and the screen flashes');
  await page.screenshot({ path: path.join(SHOTS, 'flash-settings-light.png'), fullPage: true });
  await page.locator('[data-a="back-today"]').click();
  ok((await page.locator('#restFlash').count()) === 1 && !(await h.shown()) && !(await page.locator('#restFlash').isVisible()), 'flash exists, hidden at start');
  ok((await page.locator('#restFlash').getAttribute('aria-hidden')) === 'true', 'flash hidden from screen readers (the bar already says Go)');

  // 2. Rest counting down: no flash; at zero: flash with the beep
  await page.locator('[data-a="start-day"]').click();
  await h.tick(0, 0);
  await page.waitForTimeout(1200);
  ok(!(await h.shown()), 'no flash while rest counts down');
  const o0 = await h.osc();
  await h.toZero(1200);
  ok(await h.waitShown(), 'flash shows when rest hits zero');
  ok(await page.locator('#restFlash').isVisible() && (await page.locator('#restFlash').innerText()).trim() === 'Go!', 'flash is visible and says Go!');
  ok((await h.osc()) - o0 === 3, 'beep still plays with the flash', (await h.osc()) - o0);
  const css = await page.locator('#restFlash').evaluate(el => { const c = getComputedStyle(el), b = el.getBoundingClientRect(); return { pe: c.pointerEvents, pos: c.position, anim: c.animationName, w: b.width, hgt: b.height }; });
  ok(css.pe === 'none' && css.pos === 'fixed' && css.w === 390 && css.hgt === 844, 'flash covers the whole screen and lets taps through', css);
  ok(css.anim === 'flash', 'pulsing animation by default', css.anim);
  await page.waitForTimeout(300);
  await page.screenshot({ path: path.join(SHOTS, 'flash-go-light.png') });

  // 3. Taps go through while it flashes: tick the next set
  ok(await h.shown(), 'still flashing before the tap');
  await page.fill('[data-in="r"][data-e="0"][data-s="1"]', '8');
  await page.locator('[data-a="set-done"][data-e="0"][data-s="1"]').click({ timeout: 2000 }).then(() => ok(true, 'tick works while the flash is up'), e => ok(false, 'tick works while the flash is up', String(e).slice(0, 120)));
  ok((await page.evaluate(() => S.active.entries[0].sets[1].done)) === true, 'the tapped set is ticked');

  // 4. It goes away
  await page.waitForTimeout(2700);
  ok(!(await h.shown()) && !(await page.locator('#restFlash').isVisible()), 'flash gone after ~2.5 s');

  // 5. Sound off: still flashes, no beep
  await page.evaluate(() => { S.restSound = false; save(); });
  const o1 = await h.osc();
  await h.toZero(1200);
  ok(await h.waitShown(), 'sound off: flash still shows at zero');
  ok((await h.osc()) === o1, 'sound off: no beep', (await h.osc()) - o1);
  await page.evaluate(() => { S.restSound = true; save(); });
  await page.waitForTimeout(2700);

  // 6. Skip: no flash later
  await h.tick(0, 2);
  await page.locator('[data-a="rest-skip"]').click();
  await page.waitForTimeout(1200);
  ok(!(await page.locator('#restbar.show').count()) && !(await h.shown()), 'Skip: bar and flash gone');

  // 7. Late return: reopening long after zero shows Go on the bar, but no flash (and no beep)
  await h.tick(1, 0);
  await page.evaluate(() => { const r = S.active.rest; r.start -= r.secs * 1000 + 30000; save(); });
  await page.reload(); await page.waitForTimeout(1500);
  ok((await page.locator('#restbar.go').count()) === 1 && !(await h.shown()) && (await h.osc()) === 0, 'late return: Go bar, no flash, no beep');
  ok(errors.length === 0, 'no page errors', errors);

  // 8. Reduced motion: a steady fade, no blinking
  const rm = await run('light', 'reduce'); const hr = helpers(rm.page);
  await rm.page.locator('[data-a="start-day"]').click(); await hr.tick(0, 0); await hr.toZero(1200);
  ok(await hr.waitShown(), 'reduced motion: flash shows');
  ok((await rm.page.locator('#restFlash').evaluate(el => getComputedStyle(el).animationName)) === 'fade', 'reduced motion: steady fade instead of pulsing');
  ok(rm.errors.length === 0, 'no page errors (reduced motion)', rm.errors);

  // 9. Dark mode
  const d = await run('dark'); const hd = helpers(d.page);
  await d.page.locator('[data-a="start-day"]').click(); await hd.tick(0, 0); await hd.toZero(1200);
  ok(await hd.waitShown(), 'dark: flash shows');
  await d.page.waitForTimeout(300);
  await d.page.screenshot({ path: path.join(SHOTS, 'flash-go-dark.png') });
  ok(d.errors.length === 0, 'no page errors (dark)', d.errors);

  console.log(`\n${pass} passed, ${fail} failed`);
  await browser.close(); server.close(); process.exit(fail ? 1 : 0);
})().catch(e => { console.error(e); process.exit(1); });

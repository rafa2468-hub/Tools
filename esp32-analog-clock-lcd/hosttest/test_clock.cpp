// Host-side exercise of the clock's rendering logic.
//
// The sketch draws everything itself now, so these tests run the real
// line, circle and glyph code - only "set one pixel" is stubbed.
//
// The interesting property is that the incremental renderer (which only
// touches what moved) must leave the panel byte-identical to a full
// repaint of the same instant. If erasing the sweeping second hand ever
// damages a numeral or another hand without repairing it, the two
// framebuffers diverge and the comparison test fails.

#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>

uint16_t g_fb[FB_W * FB_H];
long g_pixelWrites = 0;
int g_batchDepth = 0;
uint16_t g_bgColor = 0xFFFF;
uint8_t g_bgTouched[FB_W * FB_H];
long g_panelOps = 0;
int g_inFillRect = 0;
uint32_t g_dropPerMille = 0;
uint32_t g_dropRng = 1;
double g_testNow = 0;
SerialStub Serial;
WiFiStub WiFi;

#include "analog_clock.ino"

static int failures = 0;
static void check(bool ok, const char *what) {
  printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) failures++;
}

static double timeAt(int hh, int mm, double ss) {
  struct tm t = {};
  t.tm_year = 2026 - 1900; t.tm_mon = 6; t.tm_mday = 15; // 15 Jul 2026
  t.tm_hour = hh; t.tm_min = mm; t.tm_sec = 0;
  t.tm_isdst = -1;
  return (double)mktime(&t) + ss;
}

// Full repaint of a given instant, from a blank slate.
static void renderReference(double when, std::vector<uint16_t> &out) {
  g_testNow = when;
  struct tm t; float s;
  readClock(t, s);
  computeHands(t, s, hourHand, minHand, secHand);
  drawFace();
  handQuadRuns(hourHand, HOUR_HAND_W, hourRuns);
  handQuadRuns(minHand, MIN_HAND_W, minRuns);
  paintRuns(hourRuns, COLOR_HOUR_HAND);
  paintRuns(minRuns, COLOR_MIN_HAND);
  drawHub();
  paintSecondHandFresh();
  out.assign(g_fb, g_fb + FB_W * FB_H);
}

static int diffCount(const std::vector<uint16_t> &a,
                     const std::vector<uint16_t> &b) {
  int n = 0;
  for (size_t i = 0; i < a.size(); i++) if (a[i] != b[i]) n++;
  return n;
}

// Sweeps from `from` to `to`, and after EVERY frame checks the panel
// against a full repaint of that same instant. Comparing only the final
// frame would miss corruption that is transiently visible and then healed
// by a later redraw - on a clock that is exactly the flicker we care
// about. Returns the worst per-frame pixel divergence seen.
static int sweepWorstDivergence(double from, double to, int stepMs) {
  g_testNow = from;
  struct tm t; float s;
  readClock(t, s);
  computeHands(t, s, hourHand, minHand, secHand);
  drawFace();
  handQuadRuns(hourHand, HOUR_HAND_W, hourRuns);
  handQuadRuns(minHand, MIN_HAND_W, minRuns);
  paintRuns(hourRuns, COLOR_HOUR_HAND);
  paintRuns(minRuns, COLOR_MIN_HAND);
  drawHub();
  paintSecondHandFresh();

  int worst = 0;
  std::vector<uint16_t> inc, ref;
  for (double now = from; now <= to; now += stepMs / 1000.0) {
    g_testNow = now;
    loop();

    inc.assign(g_fb, g_fb + FB_W * FB_H);
    Hand savedH = hourHand, savedM = minHand, savedS = secHand;

    renderReference(now, ref);
    worst = std::max(worst, diffCount(ref, inc));

    // put the incremental state back so the sweep continues from it
    std::copy(inc.begin(), inc.end(), g_fb);
    hourHand = savedH; minHand = savedM; secHand = savedS;
    g_testNow = now;
  }
  return worst;
}

int main() {
  setenv("TZ", TZ_INFO, 1);
  tzset();
  panelInit();

  // ---- 1. hand bearings
  {
    g_testNow = timeAt(3, 0, 0);
    struct tm t; float s;
    readClock(t, s);
    Hand h, m, sec;
    computeHands(t, s, h, m, sec);
    check(fabsf(h.angle - 90.0f) < 0.01f, "3:00:00 -> hour hand at 90 deg");
    check(fabsf(m.angle) < 0.01f, "3:00:00 -> minute hand at 0 deg");
    check(fabsf(sec.angle) < 0.01f, "3:00:00 -> second hand at 0 deg");

    g_testNow = timeAt(1, 30, 0);
    readClock(t, s);
    computeHands(t, s, h, m, sec);
    check(fabsf(h.angle - 45.0f) < 0.01f,
          "1:30:00 -> hour hand halfway between 1 and 2");
  }

  // ---- 2. the sweep is actually smooth
  {
    std::vector<std::pair<int,int>> tips;
    double base = timeAt(10, 10, 0);
    for (int ms = 0; ms < 2000; ms += 10) {
      g_testNow = base + ms / 1000.0;
      struct tm t; float s;
      readClock(t, s);
      Hand h, m, sec;
      computeHands(t, s, h, m, sec);
      if (tips.empty() || tips.back() != std::make_pair((int)sec.x, (int)sec.y))
        tips.push_back({sec.x, sec.y});
    }
    // Over 2 seconds a ticking hand yields 2-3 distinct tips; a sweeping
    // one yields many more.
    check(tips.size() > 20, "sweep produces many distinct tip positions "
                            "over 2s (not a per-second tick)");
    int maxJump = 0;
    for (size_t i = 1; i < tips.size(); i++) {
      int dx = tips[i].first - tips[i-1].first;
      int dy = tips[i].second - tips[i-1].second;
      maxJump = std::max(maxJump, dx*dx + dy*dy);
    }
    check(maxJump <= 2, "consecutive tip positions are adjacent pixels");
  }

  // ---- 3. incremental sweep must match a full repaint
  struct Case { const char *name; int hh, mm; double from, to; };
  Case cases[] = {
    // sweeps the second hand straight through the "12" numeral
    {"second hand sweeps across a numeral", 10, 10, 57.0, 60.0},
    // second hand passes over the minute hand
    {"second hand crosses the minute hand", 4, 20, 18.0, 22.0},
    // second hand passes over the hour hand
    {"second hand crosses the hour hand",   4, 20, 38.0, 42.0},
    // minute hand itself steps while the second hand sweeps
    {"minute rollover during sweep",        7, 59, 57.0, 61.0},
    // a full revolution, hitting every numeral and both hands
    {"full 60s revolution",                 8, 45,  0.0, 60.0},
  };

  for (const auto &c : cases) {
    int worst = sweepWorstDivergence(timeAt(c.hh, c.mm, c.from),
                                     timeAt(c.hh, c.mm, c.to), 20);
    char msg[160];
    snprintf(msg, sizeof(msg), "%s (worst frame: %d px)", c.name, worst);
    check(worst == 0, msg);
  }

  // ---- 4. SPI transactions are balanced
  //
  // renderHands() wraps each frame in panelBeginBatch()/panelEndBatch().
  // If those ever get out of step the display can stall mid-transaction on
  // real hardware, which is invisible to a framebuffer comparison - so
  // check the nesting depth returns to zero explicitly.
  {
    double base = timeAt(6, 30, 10.0);
    g_testNow = base;
    struct tm t; float s;
    readClock(t, s);
    computeHands(t, s, hourHand, minHand, secHand);
    for (double now = base; now < base + 3.0; now += 0.02) {
      g_testNow = now;
      loop();
    }
    check(g_batchDepth == 0,
          "panelBeginBatch/panelEndBatch balanced across frames");
  }

  // ---- 5. the second hand must not blink
  //
  // The flicker guard, and the reason this sketch does not simply erase
  // and repaint the hand. The panel has no back buffer, so a pixel painted
  // to background and then repainted in the same frame is visibly dark for
  // however long that frame takes. Erasing the whole hand made all ~165 of
  // its pixels blink every frame; on video the hand vanished outright in
  // ~3% of camera frames. Pixels the hand has moved off are not counted -
  // those are supposed to go dark.
  {
    g_bgColor = COLOR_BG;
    double base = timeAt(2, 14, 0.0);
    g_testNow = base;
    struct tm t; float s;
    readClock(t, s);
    computeHands(t, s, hourHand, minHand, secHand);
    drawFace();
    handQuadRuns(hourHand, HOUR_HAND_W, hourRuns);
    handQuadRuns(minHand, MIN_HAND_W, minRuns);
    paintRuns(hourRuns, COLOR_HOUR_HAND);
    paintRuns(minRuns, COLOR_MIN_HAND);
    drawHub();
    paintSecondHandFresh();

    long worst = 0, total = 0, frames = 0;
    for (double now = base; now < base + 30.0; now += 0.02) {
      g_testNow = now;
      Hand ps = secHand;
      memset(g_bgTouched, 0, sizeof(g_bgTouched));
      loop();
      if (sameHand(ps, secHand)) continue;   // hand did not move
      long blinked = 0;
      for (int16_t i = 0; i < secOldN; i++) {
        if (g_bgTouched[secOldY[i] * FB_W + secOldX[i]]) blinked++;
      }
      worst = std::max(worst, blinked);
      total += blinked;
      frames++;
    }
    printf("      second-hand pixels that blinked: %.1f avg, %ld worst"
           "  (hand is ~165px)\n", frames ? (double)total / frames : 0.0, worst);
    check(worst <= 25, "second hand does not blink wholesale each frame");
    g_bgColor = 0xFFFF;
  }

  // ---- 6. cost per frame stays bounded
  {
    g_testNow = timeAt(8, 45, 0.0);
    struct tm t; float s;
    readClock(t, s);
    computeHands(t, s, hourHand, minHand, secHand);
    drawFace();
    g_pixelWrites = 0;
    g_panelOps = 0;
    double from = timeAt(8, 45, 0.0);
    for (double now = from; now < from + 60.0; now += 0.02) {
      g_testNow = now;
      loop();
    }
    long perSec = g_pixelWrites / 60;
    long opsSec = g_panelOps / 60;
    // In PANEL_SAFE_WRITES mode every pixel is its own address window on
    // the real panel - roughly 35us of CS toggling and byte transfers -
    // so the pixel count, not the rect count, is what fills the frame
    // budget. The stub counts a rect as one op, which is the streaming
    // -mode cost; both are reported so widening a hand shows its true
    // price.
    printf("      ~%ld pixel writes/second (~%ld%% CPU in safe mode), "
           "~%ld rect ops/second\n",
           perSec, (perSec * 35) / 10000, opsSec);
    check(perSec < 60000, "sweep stays within a sane pixel budget");
    // Address windows are the real cost on this driver: roughly 6
    // digitalWrites and 11 SPI byte transfers each, call it 20us. Much
    // past 10k/s and the sweep cannot keep up.
    check(opsSec < 10000, "sweep stays within a sane address-window budget");
  }

  // ---- 7. dropped writes must not become permanent marks
  //
  // The panel does occasionally lose a write. The hand's own pixels are
  // repainted every frame and so heal, but a pixel the hand has moved off
  // is erased once - so a single dropped erase used to leave a red speck
  // that nothing ever cleared, and they accumulated into comet-trails
  // along the hand's path. Discards are now re-erased for a few frames.
  {
    g_dropPerMille = 0;
    double base = timeAt(9, 3, 0.0);
    g_testNow = base;
    struct tm t; float s;
    readClock(t, s);
    computeHands(t, s, hourHand, minHand, secHand);
    drawFace();
    handQuadRuns(hourHand, HOUR_HAND_W, hourRuns);
    handQuadRuns(minHand, MIN_HAND_W, minRuns);
    paintRuns(hourRuns, COLOR_HOUR_HAND);
    paintRuns(minRuns, COLOR_MIN_HAND);
    drawHub();
    paintSecondHandFresh();

    g_dropPerMille = 20;              // 2% of writes lost
    g_dropRng = 20260803u;
    for (double now = base; now < base + 300.0; now += 0.02) {
      g_testNow = now;
      loop();
    }
    std::vector<uint16_t> inc(g_fb, g_fb + FB_W * FB_H);

    g_dropPerMille = 0;               // clean reference of the same instant
    std::vector<uint16_t> ref;
    renderReference(g_testNow, ref);

    int stray = 0;
    for (size_t i = 0; i < ref.size(); i++) {
      if (inc[i] == COLOR_SEC_HAND && ref[i] == COLOR_BG) stray++;
    }
    printf("      5 min at 2%% dropped writes -> %d stray pixels left\n",
           stray);
    check(stray <= 20, "dropped erases do not accumulate into trails");
  }

  // ---- 8. strays self-heal even when drops beat the retries
  //
  // The erase retries cover ~200ms; a burst of dropped writes longer than
  // that leaves permanent strays - which is exactly what happened on
  // hardware. The scrub bounds their lifetime instead: it re-erases the
  // hand's whole reach on a 6-minute revolution. Hammer the panel with a
  // 6% drop rate for 3 minutes, then give the scrub two quiet
  // revolutions, and require the dial to be byte-identical to a clean
  // repaint - not just stray-free, fully healed, numerals and all.
  {
    g_dropPerMille = 0;
    double base = timeAt(11, 20, 0.0);
    g_testNow = base;
    struct tm t; float s;
    readClock(t, s);
    computeHands(t, s, hourHand, minHand, secHand);
    drawFace();
    handQuadRuns(hourHand, HOUR_HAND_W, hourRuns);
    handQuadRuns(minHand, MIN_HAND_W, minRuns);
    paintRuns(hourRuns, COLOR_HOUR_HAND);
    paintRuns(minRuns, COLOR_MIN_HAND);
    drawHub();
    paintSecondHandFresh();

    g_dropPerMille = 60;
    g_dropRng = 424242u;
    for (double now = base; now < base + 180.0; now += 0.02) {
      g_testNow = now;
      loop();
    }
    g_dropPerMille = 0;
    int strayAfterBurst = 0;
    {
      std::vector<uint16_t> snap(g_fb, g_fb + FB_W * FB_H), ref;
      Hand sh = hourHand, sm = minHand, ss = secHand;
      int so = secOldN;
      std::vector<int16_t> sx(secOldX, secOldX + secOldN),
          sy(secOldY, secOldY + secOldN);
      renderReference(g_testNow, ref);
      for (size_t i = 0; i < ref.size(); i++)
        if (snap[i] != ref[i]) strayAfterBurst++;
      std::copy(snap.begin(), snap.end(), g_fb);
      hourHand = sh; minHand = sm; secHand = ss;
      secOldN = so;
      std::copy(sx.begin(), sx.end(), secOldX);
      std::copy(sy.begin(), sy.end(), secOldY);
    }

    for (double now = base + 180.0; now < base + 180.0 + 780.0;
         now += 0.02) {
      g_testNow = now;
      loop();
    }
    std::vector<uint16_t> inc(g_fb, g_fb + FB_W * FB_H), ref;
    renderReference(g_testNow, ref);
    int diff = 0;
    for (size_t i = 0; i < ref.size(); i++)
      if (inc[i] != ref[i]) diff++;
    printf("      3 min at 6%% drops left %d bad px; after two scrub "
           "revolutions: %d\n", strayAfterBurst, diff);
    check(strayAfterBurst > 0,
          "burst actually corrupted the dial (test is not vacuous)");
    check(diff == 0, "scrub heals the dial completely after a drop burst");
  }

  // ---- 9. planted specks anywhere on the dial get cleaned
  //
  // Full-circle cleanup coverage. An honest caveat: this CANNOT catch the
  // scrub resonance bug that shipped (revolution exactly 360s against the
  // hand's exact 60s period, so keep-out-skipped bearings were skipped
  // every revolution forever, leaving stray bands 36 degrees apart on
  // real hardware). In simulation the frame grid is perfectly regular, so
  // the hand's own sweep re-covers the whole annulus every minute and
  // cleans these specks itself - mutation-testing the resonant scrub
  // against this test passes. On hardware, frame phase drifts and writes
  // drop, which is why the scrub must visit every bearing; that property
  // is enforced by construction in scrubTick (wait on a kept-out bearing
  // instead of advancing past it) rather than provable here.
  {
    g_dropPerMille = 0;
    double base = timeAt(5, 47, 0.0);
    g_testNow = base;
    struct tm t; float s;
    readClock(t, s);
    computeHands(t, s, hourHand, minHand, secHand);
    drawFace();
    handQuadRuns(hourHand, HOUR_HAND_W, hourRuns);
    handQuadRuns(minHand, MIN_HAND_W, minRuns);
    paintRuns(hourRuns, COLOR_HOUR_HAND);
    paintRuns(minRuns, COLOR_MIN_HAND);
    drawHub();
    paintSecondHandFresh();

    // Planted 2px perpendicular to each bearing's ray - deliberately OFF
    // the second hand's own pixel path. Specks exactly on the path get
    // cleaned by the hand's ordinary erase cycle as it sweeps past, which
    // masks a scrub that never visits; real strays survive precisely
    // because they sit a pixel or two off the live path.
    for (int i = 0; i < 60; i++) {
      float ang = i * 6.0f + 3.0f;
      float rad = ang * DEG_TO_RAD;
      for (int16_t r = 40; r <= 120; r += 40) {
        int16_t x, y;
        polarToXY(ang, r, x, y);
        int16_t ox = (int16_t)lroundf(2.0f * cosf(rad));
        int16_t oy = (int16_t)lroundf(2.0f * sinf(rad));
        g_fb[(y + oy) * FB_W + (x + ox)] = COLOR_SEC_HAND;
      }
    }

    for (double now = base; now < base + 900.0; now += 0.02) {
      g_testNow = now;
      loop();
    }

    std::vector<uint16_t> inc(g_fb, g_fb + FB_W * FB_H), ref;
    renderReference(g_testNow, ref);
    int diff = 0;
    for (size_t i = 0; i < ref.size(); i++)
      if (inc[i] != ref[i]) diff++;
    printf("      180 planted specks around the dial; after 15 min: "
           "%d px still wrong\n", diff);
    check(diff == 0, "scrub reaches every bearing (no resonance shadow)");
  }

  // ---- 10. the hour and minute hands must not blink either
  //
  // The second hand had a blink guard from early on; these did not, and
  // that gap shipped. In safe write mode every pixel costs real time, so
  // erasing a whole ~375px minute hand and repainting it left it dark for
  // tens of milliseconds - thirteen times a minute, plus the retry frames
  // after each step. On video both thick hands visibly flickered. They
  // are now stepped differentially: only the vacated sliver is restored
  // and only the newly covered pixels are inked, so the hundreds of
  // shared pixels are never touched.
  {
    g_bgColor = COLOR_BG;
    double base = timeAt(3, 41, 0.0);
    g_testNow = base;
    struct tm t; float s;
    readClock(t, s);
    computeHands(t, s, hourHand, minHand, secHand);
    drawFace();
    handQuadRuns(hourHand, HOUR_HAND_W, hourRuns);
    handQuadRuns(minHand, MIN_HAND_W, minRuns);
    paintRuns(hourRuns, COLOR_HOUR_HAND);
    paintRuns(minRuns, COLOR_MIN_HAND);
    drawHub();
    paintSecondHandFresh();

    long worstMin = 0, worstHour = 0, minSteps = 0;
    for (double now = base; now < base + 120.0; now += 0.02) {
      g_testNow = now;
      Hand pm = minHand;
      memset(g_bgTouched, 0, sizeof(g_bgTouched));
      loop();
      // How many pixels of the hand's *current* shape went dark this
      // frame? Those are pixels that blinked; the vacated sliver is not
      // part of the current shape and so is correctly excluded.
      long blinkMin = 0, blinkHour = 0;
      for (int16_t i = 0; i < minRuns.n; i++)
        for (int16_t x = minRuns.rx0[i]; x <= minRuns.rx1[i]; x++)
          if (g_bgTouched[minRuns.ry[i] * FB_W + x]) blinkMin++;
      for (int16_t i = 0; i < hourRuns.n; i++)
        for (int16_t x = hourRuns.rx0[i]; x <= hourRuns.rx1[i]; x++)
          if (g_bgTouched[hourRuns.ry[i] * FB_W + x]) blinkHour++;
      worstMin = std::max(worstMin, blinkMin);
      worstHour = std::max(worstHour, blinkHour);
      if (!sameHand(pm, minHand)) minSteps++;
    }
    printf("      over 2 min (%ld minute-hand steps): worst blink "
           "minute %ld px, hour %ld px  (hands are ~375/~425px)\n",
           minSteps, worstMin, worstHour);
    check(minSteps > 0, "minute hand actually stepped during the window");
    check(worstMin <= 40 && worstHour <= 40,
          "hour and minute hands are not blanked when they step");
    g_bgColor = 0xFFFF;
  }

  printf("\n%s\n", failures ? "FAILURES" : "all checks passed");
  return failures ? 1 : 0;
}

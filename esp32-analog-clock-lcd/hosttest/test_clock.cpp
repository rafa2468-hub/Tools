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
  paintHand(hourHand, COLOR_HOUR_HAND, HOUR_HAND_W);
  paintHand(minHand, COLOR_MIN_HAND, MIN_HAND_W);
  drawHub();
  paintHand(secHand, COLOR_SEC_HAND, SEC_HAND_W);
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
  paintHand(hourHand, COLOR_HOUR_HAND, HOUR_HAND_W);
  paintHand(minHand, COLOR_MIN_HAND, MIN_HAND_W);
  drawHub();
  paintHand(secHand, COLOR_SEC_HAND, SEC_HAND_W);

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

  // ---- 5. cost per frame stays bounded
  {
    g_testNow = timeAt(8, 45, 0.0);
    struct tm t; float s;
    readClock(t, s);
    computeHands(t, s, hourHand, minHand, secHand);
    drawFace();
    g_pixelWrites = 0;
    double from = timeAt(8, 45, 0.0);
    for (double now = from; now < from + 60.0; now += 0.02) {
      g_testNow = now;
      loop();
    }
    long perSec = g_pixelWrites / 60;
    printf("      ~%ld pixel writes/second during sweep\n", perSec);
    check(perSec < 60000, "sweep stays within a sane pixel budget");
  }

  printf("\n%s\n", failures ? "FAILURES" : "all checks passed");
  return failures ? 1 : 0;
}

// Analog clock for a 2.1" round 360x360 GC9B72 SPI TFT, driven by an
// ESP32-C3 Super Mini.
//
// This sketch depends on NO graphics library. Two attempts to reuse an
// existing driver for this panel failed on real hardware - Arduino_GFX's
// GC9C01 (GalaxyCore's two 360x360 round controllers turn out not to be
// register-compatible) and then LovyanGFX - so everything drawn here is
// built from two primitives: fill a rectangle, and set one pixel. Those
// live in the PANEL ADAPTER block below, wired to GC9B72Graphics.hpp, and
// are the only code in this file that knows what the display is.
//
// Time is obtained over Wi-Fi via NTP (the ESP32-C3 has no RTC of its
// own). If Wi-Fi is not configured or the connection attempt fails, the
// clock falls back to the firmware's compile timestamp and free-runs from
// there so it still displays something useful.

#include <WiFi.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// ---------------------------------------------------------------------
// User configuration
// ---------------------------------------------------------------------

// Wi-Fi credentials used only to fetch the time over NTP. Leave the SSID
// at its placeholder value to skip Wi-Fi entirely and run from the
// compile-time fallback clock.
static const char *WIFI_SSID = "YOUR_WIFI_SSID";
static const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// Local time zone as a POSIX TZ string: Europe/Warsaw, i.e. CET (UTC+1)
// switching to CEST (UTC+2) on the last Sunday of March at 02:00 and back
// on the last Sunday of October at 03:00.
//
// Note this is deliberately not the configTime(gmtOffset, daylightOffset)
// form. That one builds a TZ string with no DST transition rule, leaving
// the C library to apply its built-in default - US switching dates, which
// are two weeks off from the EU's in spring and a week off in autumn. An
// explicit rule string keeps the DST changeover correct year-round with
// no code change.
static const char *TZ_INFO = "CET-1CEST,M3.5.0,M10.5.0/3";

// Time source. The primary is a local NTP server on the LAN, so the clock
// syncs without needing to reach the internet. The secondary is a public
// pool, used only if the local server doesn't answer - drop it (set to
// nullptr) if this device should never talk to the outside world.
static const char *NTP_SERVER_1 = "192.168.1.5";
static const char *NTP_SERVER_2 = "pool.ntp.org";

// true  - the second hand sweeps continuously, using sub-second time.
// false - it steps once per second, the way a quartz movement does.
static const bool SMOOTH_SECONDS = true;

// ---------------------------------------------------------------------
// Hand state, tracked so each redraw only touches what changed
// ---------------------------------------------------------------------
//
// Declared here, above everything else, because of how the Arduino IDE
// builds a .ino: it generates prototypes for every function in the sketch
// and injects them immediately before the *first* function definition in
// the file. Any type named in a signature has to already exist at that
// point, so this struct must sit above the first function - which today
// means above the panel adapter, not merely above the drawing code that
// uses it. Get this wrong and the IDE emits a wall of "'Hand' has not
// been declared" pointing at lines that look perfectly fine.
//
// PlatformIO and the host tests compile the .ino as ordinary C++ and
// never reorder anything, so they stay green either way. hosttest's
// ino-check target is what actually guards this.
struct Hand {
  int16_t x, y;   // tip
  int16_t tx, ty; // tail (the center, except for the second hand)
  float angle;    // bearing of the tip, degrees clockwise from 12
};
static Hand hourHand, minHand, secHand;

// ---------------------------------------------------------------------
// Hour/minute hands as scanline runs
// ---------------------------------------------------------------------
// (Declared up here with struct Hand for the same Arduino IDE
// prototype-injection reason: these types appear in function signatures,
// so they must exist above the first function definition in the file.
// The ino-check target caught exactly this when they lived lower down.)
//
// The thick hands are tracked as the exact horizontal runs they were
// painted with, so a step can erase precisely the vacated pixels (old
// minus new) and paint precisely the uncovered ones (new minus old).
// In safe write mode every pixel costs real time, and the difference
// between "repaint a 400-pixel hand" and "touch the 20 pixels that
// changed" is the difference between a smooth dial and a flickering one:
// a hand erased wholesale is dark from its erase until its repaint, and
// the camera - and the eye - catch that window.
static const int16_t HAND_RUNS_MAX = 160;
struct HandRuns {
  int16_t n;
  int16_t ry[HAND_RUNS_MAX], rx0[HAND_RUNS_MAX], rx1[HAND_RUNS_MAX];
};
static HandRuns hourRuns, minRuns; // as currently painted on the panel

// Vacated slivers being re-erased for a few extra frames (dropped-write
// insurance, same idea as the second hand's generations).
static const int16_t SLIVER_MAX = 2 * HAND_RUNS_MAX;
struct Sliver {
  int16_t n;
  int16_t y[SLIVER_MAX], x0[SLIVER_MAX], x1[SLIVER_MAX];
};
static Sliver hourSliver, minSliver;
static int8_t hourSliverLeft = 0, minSliverLeft = 0;

// Panel size. Everything on the face is positioned from these, so a
// different round panel only needs these two numbers changed.
static const int16_t PANEL_W = 360;
static const int16_t PANEL_H = 360;

// =====================================================================
// PANEL ADAPTER
// =====================================================================
// The only code in this sketch that knows what the display is. Wired
// here to GC9B72Graphics.hpp; everything below draws through these and
// needs no changes if the driver is swapped.
//
// panelFillRect is not a convenience - it is what makes the sweep
// possible. The clock draws in horizontal runs, and on this driver a run
// costs one address window instead of one per pixel.

#if HOSTTEST
// The test harness supplies its own framebuffer-backed implementations.
void panelInit();
void panelDrawPixel(int16_t x, int16_t y, uint16_t color);
void panelFillRect(int16_t x, int16_t y, int16_t w, int16_t h,
                   uint16_t color);
void panelBeginBatch();
void panelEndBatch();
#else

#include <SPI.h>
#include "GC9B72Graphics.hpp" // the working GC9B72 driver: pins, init
                              // sequence, sendCmd/sendData, setAddrWindow

// SPI clock for pixel pushing.
//
// GC9B72Graphics.hpp never calls beginTransaction, so the vendor demo ran
// at the Arduino default of 1MHz. 40MHz was tried here and was not the
// cause of the stray pixels this sketch once left behind - that was a
// missing erase retry, fixed in renderHands - but there is no reason to
// run fast either. A frame's cost is dominated by per-window GPIO
// overhead, not clocking: at 10MHz an address window's 11 bytes take
// under 9us and the sweep needs ~7500 of them a second, so under 10% of
// the time budget. A slower bus simply drops fewer writes, and every
// dropped write is one the retry logic has to clean up.
static const uint32_t PANEL_SPI_HZ = 10000000;
static SPISettings panelSpi(PANEL_SPI_HZ, MSBFIRST, SPI_MODE0);

// Opens an address window and leaves CS asserted with DC high, ready for
// the caller to stream pixel data and then raise CS.
//
// This deliberately does not reuse the driver's setAddrWindow/sendCmd/
// sendData. Those toggle CS around every individual byte, which costs
// about 22 digitalWrite calls per window - fine for a one-off fill, far
// too slow when the sweep needs one every few pixels. Holding CS low
// across the whole command sequence is the normal way to drive these
// controllers and cuts that to six.
static inline void panelOpenWindow(uint16_t x0, uint16_t y0, uint16_t x1,
                                    uint16_t y1) {
  digitalWrite(PIN_CS, LOW);

  digitalWrite(PIN_DC, LOW);
  SPI.transfer(0x2A);
  digitalWrite(PIN_DC, HIGH);
  SPI.transfer(x0 >> 8); SPI.transfer(x0 & 0xFF);
  SPI.transfer(x1 >> 8); SPI.transfer(x1 & 0xFF);

  digitalWrite(PIN_DC, LOW);
  SPI.transfer(0x2B);
  digitalWrite(PIN_DC, HIGH);
  SPI.transfer(y0 >> 8); SPI.transfer(y0 & 0xFF);
  SPI.transfer(y1 >> 8); SPI.transfer(y1 & 0xFF);

  digitalWrite(PIN_DC, LOW);
  SPI.transfer(0x2C); // memory write
  digitalWrite(PIN_DC, HIGH);
}

static inline void panelCloseWindow() { digitalWrite(PIN_CS, HIGH); }

static void panelInit() {
  // GC9B72Graphics.hpp only sends the register sequence - the bus and pins
  // are the caller's job, so they are set up here.
  pinMode(PIN_CS, OUTPUT);
  digitalWrite(PIN_CS, HIGH);
  pinMode(PIN_DC, OUTPUT);
  digitalWrite(PIN_DC, HIGH);
  pinMode(PIN_RST, OUTPUT);

  digitalWrite(PIN_RST, HIGH);
  delay(10);
  digitalWrite(PIN_RST, LOW);
  delay(20);
  digitalWrite(PIN_RST, HIGH);
  delay(120);

  SPI.begin(PIN_SCK, -1 /* MISO unused */, PIN_MOSI, -1 /* CS by hand */);
  SPI.beginTransaction(panelSpi);
  gc9b72_init();
  SPI.endTransaction();
}

// Write-path selection.
//
// 1 (safe): every pixel goes through the vendor driver's own drawPixel -
//   full address window per pixel, CS toggled around every byte, exactly
//   the write pattern the module's demo used and the one thing this panel
//   has demonstrably executed reliably. Slower, but measured against the
//   frame budget it still fits several times over; the visible cost is
//   the boot background fill taking ~2-3s.
//
// 0 (fast): one address window per rectangle, pixels streamed in bulk
//   with CS held low. This is how SPI TFTs are conventionally driven and
//   the boot self-test passes through it - but it is also the one part of
//   this sketch the vendor demo never exercised, and residue observed on
//   the running clock (unerased trailing pixels of the minute hand,
//   specks recurring in fixed zones) is consistent with this path
//   intermittently failing under load while per-pixel writes keep
//   working: the second hand, whose erase is per-pixel, visibly cleans up
//   after the streaming erases. Until the streaming path is proven clean
//   on this hardware, safe mode is the default.
#define PANEL_SAFE_WRITES 1

static void panelDrawPixel(int16_t x, int16_t y, uint16_t color) {
  if (x < 0 || y < 0 || x >= PANEL_W || y >= PANEL_H) return;
#if PANEL_SAFE_WRITES
  drawPixel(x, y, color); // the vendor driver's own, known-good on glass
#else
  panelOpenWindow(x, y, x, y);
  SPI.transfer(color >> 8);
  SPI.transfer(color & 0xFF);
  panelCloseWindow();
#endif
}

static void panelFillRect(int16_t x, int16_t y, int16_t w, int16_t h,
                          uint16_t color) {
  // Clip to the panel; the drawing code above happily runs off the edge.
  if (w <= 0 || h <= 0) return;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > PANEL_W) w = PANEL_W - x;
  if (y + h > PANEL_H) h = PANEL_H - y;
  if (w <= 0 || h <= 0) return;

#if PANEL_SAFE_WRITES
  for (int16_t j = 0; j < h; j++) {
    for (int16_t i = 0; i < w; i++) {
      panelDrawPixel(x + i, y + j, color);
    }
  }
#else
  // One window for the whole rectangle, then stream. Sending a prefilled
  // buffer in bulk rather than two SPI.transfer() calls per pixel is what
  // makes the 129,600-pixel background fill quick rather than glacial.
  static uint8_t buf[128]; // 64 pixels
  for (uint16_t i = 0; i < sizeof(buf); i += 2) {
    buf[i] = color >> 8;
    buf[i + 1] = color & 0xFF;
  }
  uint32_t remaining = (uint32_t)w * h;
  panelOpenWindow(x, y, x + w - 1, y + h - 1);
  while (remaining) {
    uint32_t chunk = remaining > 64 ? 64 : remaining;
    SPI.writeBytes(buf, chunk * 2);
    remaining -= chunk;
  }
  panelCloseWindow();
#endif
}

// One SPI transaction per frame rather than one per primitive.
static void panelBeginBatch() { SPI.beginTransaction(panelSpi); }
static void panelEndBatch() { SPI.endTransaction(); }

#endif

// ---------------------------------------------------------------------
// Clock face geometry
// ---------------------------------------------------------------------
static const int16_t CX = 180;
static const int16_t CY = 180;
static const int16_t RIM_OUTER_R = 178;
static const int16_t RIM_INNER_R = 174;
static const int16_t HOUR_TICK_OUTER_R = 170;
static const int16_t HOUR_TICK_INNER_R = 150;
static const int16_t MIN_TICK_OUTER_R = 170;
static const int16_t MIN_TICK_INNER_R = 160;
static const int16_t NUMERAL_R = 134;

static const int16_t HOUR_HAND_LEN = 85;
static const int16_t MIN_HAND_LEN = 125;
static const int16_t SEC_HAND_LEN = 145;
static const int16_t SEC_HAND_TAIL = 20; // short tail past the center

static const uint8_t HOUR_HAND_W = 5;
static const uint8_t MIN_HAND_W = 3;
static const int16_t HUB_R = 6;
// The second hand has no width constant: it is a single-pixel Bresenham
// line by construction (collectLine / drawPixelList), tracked as a pixel
// list so its sweep can be updated incrementally. Widening it would mean
// giving it the scanline-run treatment the thick hands get, not changing
// a number here.

#define RGB565(r, g, b) \
  ((uint16_t)((((r)&0xF8) << 8) | (((g)&0xFC) << 3) | (((b)&0xF8) >> 3)))

static const uint16_t COLOR_BG = RGB565(0, 0, 8);
static const uint16_t COLOR_RIM = RGB565(190, 190, 200);
static const uint16_t COLOR_TICK = RGB565(225, 225, 230);
static const uint16_t COLOR_NUMERAL = RGB565(240, 240, 245);
static const uint16_t COLOR_HOUR_HAND = RGB565(240, 240, 245);
static const uint16_t COLOR_MIN_HAND = RGB565(0, 200, 255);
static const uint16_t COLOR_SEC_HAND = RGB565(255, 60, 60);
static const uint16_t COLOR_HUB = RGB565(255, 60, 60);


// ---------------------------------------------------------------------
// Drawing primitives
// ---------------------------------------------------------------------
// Standard integer algorithms, all resolving to panelDrawPixel. Having
// them here rather than in a library is what makes this sketch portable
// across display drivers.

// Bresenham.
static void gfxDrawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                         uint16_t color) {
  bool steep = abs(y1 - y0) > abs(x1 - x0);
  if (steep) {
    int16_t t;
    t = x0; x0 = y0; y0 = t;
    t = x1; x1 = y1; y1 = t;
  }
  if (x0 > x1) {
    int16_t t;
    t = x0; x0 = x1; x1 = t;
    t = y0; y0 = y1; y1 = t;
  }
  int16_t dx = x1 - x0;
  int16_t dy = abs(y1 - y0);
  int16_t err = dx / 2;
  int16_t ystep = (y0 < y1) ? 1 : -1;
  for (; x0 <= x1; x0++) {
    if (steep) {
      panelDrawPixel(y0, x0, color);
    } else {
      panelDrawPixel(x0, y0, color);
    }
    err -= dy;
    if (err < 0) {
      y0 += ystep;
      err += dx;
    }
  }
}

// Midpoint circle, outline only.
static void gfxDrawCircle(int16_t x0, int16_t y0, int16_t r,
                           uint16_t color) {
  int16_t f = 1 - r, ddF_x = 1, ddF_y = -2 * r, x = 0, y = r;
  panelDrawPixel(x0, y0 + r, color);
  panelDrawPixel(x0, y0 - r, color);
  panelDrawPixel(x0 + r, y0, color);
  panelDrawPixel(x0 - r, y0, color);
  while (x < y) {
    if (f >= 0) { y--; ddF_y += 2; f += ddF_y; }
    x++; ddF_x += 2; f += ddF_x;
    panelDrawPixel(x0 + x, y0 + y, color);
    panelDrawPixel(x0 - x, y0 + y, color);
    panelDrawPixel(x0 + x, y0 - y, color);
    panelDrawPixel(x0 - x, y0 - y, color);
    panelDrawPixel(x0 + y, y0 + x, color);
    panelDrawPixel(x0 - y, y0 + x, color);
    panelDrawPixel(x0 + y, y0 - x, color);
    panelDrawPixel(x0 - y, y0 - x, color);
  }
}

static void gfxFillCircle(int16_t x0, int16_t y0, int16_t r,
                           uint16_t color) {
  for (int16_t dy = -r; dy <= r; dy++) {
    int16_t dx = (int16_t)sqrtf((float)(r * r - dy * dy));
    panelFillRect(x0 - dx, y0 + dy, 2 * dx + 1, 1, color);
  }
}

// A 5x7 digit font - the clock face only ever spells 0-9, so carrying a
// whole font table would be dead weight. Column-major, bit 0 is the top
// row. Glyph cell is 6 columns wide including the trailing space.
static const uint8_t DIGIT_FONT[10][5] = {
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, // 0
    {0x00, 0x42, 0x7F, 0x40, 0x00}, // 1
    {0x42, 0x61, 0x51, 0x49, 0x46}, // 2
    {0x21, 0x41, 0x45, 0x4B, 0x31}, // 3
    {0x18, 0x14, 0x12, 0x7F, 0x10}, // 4
    {0x27, 0x45, 0x45, 0x45, 0x39}, // 5
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, // 6
    {0x01, 0x71, 0x09, 0x05, 0x03}, // 7
    {0x36, 0x49, 0x49, 0x49, 0x36}, // 8
    {0x06, 0x49, 0x49, 0x29, 0x1E}, // 9
};

static const uint8_t GLYPH_W = 6; // 5 columns + 1 spacing
static const uint8_t GLYPH_H = 8; // 7 rows + 1 spacing

// Draws one digit at `scale`, painting the full cell background first so
// that redrawing a numeral also erases whatever was scribbled over it.
static void gfxDrawDigit(int16_t x, int16_t y, uint8_t digit, uint8_t scale,
                          uint16_t color, uint16_t bg) {
  panelFillRect(x, y, GLYPH_W * scale, GLYPH_H * scale, bg);
  for (uint8_t col = 0; col < 5; col++) {
    uint8_t bits = DIGIT_FONT[digit][col];
    for (uint8_t row = 0; row < 7; row++) {
      if (bits & (1 << row)) {
        panelFillRect(x + col * scale, y + row * scale, scale, scale,
                      color);
      }
    }
  }
}

static void gfxDrawNumber(int16_t x, int16_t y, const char *s, uint8_t scale,
                           uint16_t color, uint16_t bg) {
  for (const char *p = s; *p; p++) {
    gfxDrawDigit(x, y, (uint8_t)(*p - '0'), scale, color, bg);
    x += GLYPH_W * scale;
  }
}

// ---------------------------------------------------------------------
// Geometry helpers
// ---------------------------------------------------------------------

// Smallest absolute angle between two bearings, in degrees (0..180).
static float angleDelta(float a, float b) {
  float d = fmodf(fabsf(a - b), 360.0f);
  return d > 180.0f ? 360.0f - d : d;
}

// angleDeg: 0 = 12 o'clock, increasing clockwise.
static void polarToXY(float angleDeg, int16_t radius, int16_t &x,
                       int16_t &y) {
  float rad = angleDeg * DEG_TO_RAD;
  x = CX + (int16_t)lroundf(radius * sinf(rad));
  y = CY - (int16_t)lroundf(radius * cosf(rad));
}

// Scanline-fills a convex quad given in float coordinates.
static void gfxFillQuad(const float *qx, const float *qy, uint16_t color) {
  float fminY = qy[0], fmaxY = qy[0];
  for (int i = 1; i < 4; i++) {
    if (qy[i] < fminY) fminY = qy[i];
    if (qy[i] > fmaxY) fmaxY = qy[i];
  }
  int16_t yStart = (int16_t)floorf(fminY);
  int16_t yEnd = (int16_t)ceilf(fmaxY);
  for (int16_t y = yStart; y <= yEnd; y++) {
    float yc = (float)y;
    float xmin = 1e9f, xmax = -1e9f;
    for (int e = 0; e < 4; e++) {
      int a = e, b = (e + 1) & 3;
      float ya = qy[a], yb = qy[b];
      if ((ya <= yc && yb > yc) || (yb <= yc && ya > yc)) {
        float t = (yc - ya) / (yb - ya);
        float x = qx[a] + t * (qx[b] - qx[a]);
        if (x < xmin) xmin = x;
        if (x > xmax) xmax = x;
      }
    }
    if (xmax < xmin) continue;
    int16_t px0 = (int16_t)lroundf(xmin);
    int16_t px1 = (int16_t)lroundf(xmax);
    // One run per scanline rather than px1-px0 separate pixel writes: on
    // a driver that opens an address window per pixel, that is the whole
    // difference between a smooth sweep and a slideshow.
    panelFillRect(px0, y, px1 - px0 + 1, 1, color);
  }
}

// A line with width, drawn as the quad the hand actually occupies.
//
// The obvious implementation - stack `thickness` Bresenham lines at
// perpendicular offsets - looks right on horizontal and vertical hands
// and falls apart on diagonals. At 45 degrees the perpendicular offset
// rounds to steps of (1,-1), i.e. 1.41px, so the stacked lines separate
// instead of merging and the hand renders as loose parallel strands with
// gaps between them. Filling the quad sidesteps the rounding entirely and
// costs no more, since it touches only pixels actually inside the shape.
static void drawThickLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                           uint16_t color, uint8_t thickness) {
  float dx = x1 - x0;
  float dy = y1 - y0;
  float len = sqrtf(dx * dx + dy * dy);
  if (len < 1.0f) {
    panelDrawPixel(x0, y0, color);
    return;
  }
  if (thickness <= 1) {
    // A 1px quad is thinner than the sample grid, so scanline filling it
    // would drop pixels. Bresenham is exactly right here.
    gfxDrawLine(x0, y0, x1, y1, color);
    return;
  }
  float half = thickness / 2.0f;
  float ox = -dy / len * half;
  float oy = dx / len * half;
  float qx[4] = {x0 + ox, x1 + ox, x1 - ox, x0 - ox};
  float qy[4] = {y0 + oy, y1 + oy, y1 - oy, y0 - oy};
  gfxFillQuad(qx, qy, color);
}

// secOfMinute carries the fraction of a second, so the second hand can be
// placed between whole-second positions.
static void computeHands(const struct tm &t, float secOfMinute, Hand &hour,
                          Hand &minute, Hand &sec) {
  float minOfHour = t.tm_min + secOfMinute / 60.0f;

  sec.angle = secOfMinute * 6.0f;
  minute.angle = minOfHour * 6.0f;
  hour.angle = (t.tm_hour % 12) * 30.0f + minOfHour * 0.5f;

  polarToXY(hour.angle, HOUR_HAND_LEN, hour.x, hour.y);
  hour.tx = CX;
  hour.ty = CY;

  polarToXY(minute.angle, MIN_HAND_LEN, minute.x, minute.y);
  minute.tx = CX;
  minute.ty = CY;

  polarToXY(sec.angle, SEC_HAND_LEN, sec.x, sec.y);
  polarToXY(sec.angle + 180.0f, SEC_HAND_TAIL, sec.tx, sec.ty);
}

static bool sameHand(const Hand &a, const Hand &b) {
  return a.x == b.x && a.y == b.y && a.tx == b.tx && a.ty == b.ty;
}

// ---------------------------------------------------------------------
// Second hand: incremental update
// ---------------------------------------------------------------------
// Two properties have to hold at once here, and they pull against each
// other.
//
// The hand must never be dark for long. Erasing the whole hand, doing the
// repairs, and only then repainting it left all ~165 of its pixels dark
// for a whole frame - visible as a stagger, and on video the hand vanished
// outright in ~3% of camera frames.
//
// But every pixel must also be rewritten every frame. The panel
// occasionally drops a write, and when each pixel is painted once and
// never revisited, a dropped erase becomes a permanent red speck. Whole
// comet-trails of them accumulated along the hand's path when this code
// erased only the pixels the new position did not reuse. The wholesale
// erase had been hiding that all along by rewriting everything ~15 times
// a second.
//
// So: erase the old position and draw the new one *interleaved*, pixel by
// pixel. Every pixel is rewritten every frame, so a dropped write is
// corrected within ~66ms - and a pixel common to both positions is dark
// only for the single write between its erase and its redraw, which is
// microseconds rather than milliseconds.

// Longest possible Bresenham run from tail to tip, plus slack.
static const int16_t SEC_PX_MAX = SEC_HAND_LEN + SEC_HAND_TAIL + 8;
static int16_t secOldX[SEC_PX_MAX], secOldY[SEC_PX_MAX];
static int16_t secNewX[SEC_PX_MAX], secNewY[SEC_PX_MAX];
static int16_t secOldN = 0, secNewN = 0;

// How many extra frames each discarded pixel is re-erased for. See the
// retry loop in renderHands.
static const int8_t ERASE_RETRIES = 2;
static int16_t eraseGenX[ERASE_RETRIES][SEC_PX_MAX];
static int16_t eraseGenY[ERASE_RETRIES][SEC_PX_MAX];
static int16_t eraseGenN[ERASE_RETRIES] = {0};
static int16_t thisEraseX[SEC_PX_MAX], thisEraseY[SEC_PX_MAX];


// Same Bresenham as gfxDrawLine, but collecting instead of drawing, so
// that what gets erased and what gets drawn can never disagree.
static void collectLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                         int16_t *xs, int16_t *ys, int16_t &n) {
  const int16_t tailX = x0, tailY = y0;
  n = 0;
  bool steep = abs(y1 - y0) > abs(x1 - x0);
  if (steep) {
    int16_t t;
    t = x0; x0 = y0; y0 = t;
    t = x1; x1 = y1; y1 = t;
  }
  if (x0 > x1) {
    int16_t t;
    t = x0; x0 = x1; x1 = t;
    t = y0; y0 = y1; y1 = t;
  }
  int16_t dx = x1 - x0, dy = abs(y1 - y0);
  int16_t err = dx / 2, ystep = (y0 < y1) ? 1 : -1;
  for (; x0 <= x1 && n < SEC_PX_MAX; x0++) {
    if (steep) { xs[n] = y0; ys[n] = x0; }
    else       { xs[n] = x0; ys[n] = y0; }
    n++;
    err -= dy;
    if (err < 0) { y0 += ystep; err += dx; }
  }

  // Normalise the direction so the list always runs tail-to-tip. The
  // swaps above order it by whichever axis is major, which flips near 45
  // degrees - and renderHands pairs old[i] with new[i], so a flip would
  // pair the tail of one with the tip of the other.
  if (n > 1) {
    int32_t d0 = (int32_t)(xs[0] - tailX) * (xs[0] - tailX) +
                 (int32_t)(ys[0] - tailY) * (ys[0] - tailY);
    int32_t dN = (int32_t)(xs[n - 1] - tailX) * (xs[n - 1] - tailX) +
                 (int32_t)(ys[n - 1] - tailY) * (ys[n - 1] - tailY);
    if (d0 > dN) {
      for (int16_t a = 0, b = n - 1; a < b; a++, b--) {
        int16_t t;
        t = xs[a]; xs[a] = xs[b]; xs[b] = t;
        t = ys[a]; ys[a] = ys[b]; ys[b] = t;
      }
    }
  }
}

static bool inPixelList(int16_t x, int16_t y, const int16_t *xs,
                         const int16_t *ys, int16_t n) {
  for (int16_t i = 0; i < n; i++) {
    if (xs[i] == x && ys[i] == y) return true;
  }
  return false;
}

// Paints a collected pixel list, merging horizontally adjacent pixels
// into single runs - one address window per run instead of per pixel.
static void drawPixelList(const int16_t *xs, const int16_t *ys, int16_t n,
                           uint16_t color) {
  int16_t i = 0;
  while (i < n) {
    int16_t j = i + 1;
    while (j < n && ys[j] == ys[i] && xs[j] == xs[j - 1] + 1) j++;
    panelFillRect(xs[i], ys[i], j - i, 1, color);
    i = j;
  }
}

// Draws the second hand from scratch and records its pixels, so that the
// next incremental update knows what is actually on the panel. Used for
// the first paint; after that renderHands maintains the list.
static void paintSecondHandFresh() {
  collectLine(secHand.tx, secHand.ty, secHand.x, secHand.y, secOldX, secOldY,
              secOldN);
  drawPixelList(secOldX, secOldY, secOldN, COLOR_SEC_HAND);
}


// Rasterises a hand's quad into horizontal runs - the same geometry
// drawThickLine paints, captured instead of drawn, so erase-by-difference
// and the painted pixels can never disagree.
static void handQuadRuns(const Hand &h, uint8_t thickness, HandRuns &out) {
  out.n = 0;
  float dx = (float)h.x - h.tx, dy = (float)h.y - h.ty;
  float len = sqrtf(dx * dx + dy * dy);
  if (len < 1.0f) {
    out.ry[0] = h.ty;
    out.rx0[0] = out.rx1[0] = h.tx;
    out.n = 1;
    return;
  }
  float half = thickness / 2.0f;
  float ox = -dy / len * half;
  float oy = dx / len * half;
  float qx[4] = {h.tx + ox, h.x + ox, h.x - ox, h.tx - ox};
  float qy[4] = {h.ty + oy, h.y + oy, h.y - oy, h.ty - oy};

  float fminY = qy[0], fmaxY = qy[0];
  for (int i = 1; i < 4; i++) {
    if (qy[i] < fminY) fminY = qy[i];
    if (qy[i] > fmaxY) fmaxY = qy[i];
  }
  for (int16_t y = (int16_t)floorf(fminY); y <= (int16_t)ceilf(fmaxY); y++) {
    float yc = (float)y;
    float xmin = 1e9f, xmax = -1e9f;
    for (int e = 0; e < 4; e++) {
      int a = e, b = (e + 1) & 3;
      float ya = qy[a], yb = qy[b];
      if ((ya <= yc && yb > yc) || (yb <= yc && ya > yc)) {
        float t = (yc - ya) / (yb - ya);
        float x = qx[a] + t * (qx[b] - qx[a]);
        if (x < xmin) xmin = x;
        if (x > xmax) xmax = x;
      }
    }
    if (xmax < xmin || out.n >= HAND_RUNS_MAX) continue;
    out.ry[out.n] = y;
    out.rx0[out.n] = (int16_t)lroundf(xmin);
    out.rx1[out.n] = (int16_t)lroundf(xmax);
    out.n++;
  }
}

static void paintRuns(const HandRuns &r, uint16_t color) {
  for (int16_t i = 0; i < r.n; i++) {
    panelFillRect(r.rx0[i], r.ry[i], r.rx1[i] - r.rx0[i] + 1, 1, color);
  }
}

// Erases old-position pixels the new position does not reuse, recording
// them for the retry frames. Convex quads have one run per scanline, so
// the difference on each row is at most two segments.
static void eraseRunsDiff(const HandRuns &oldR, const HandRuns &newR,
                           Sliver &out);

static void addSliverSeg(Sliver &out, int16_t y, int16_t x0, int16_t x1) {
  if (x1 < x0 || out.n >= SLIVER_MAX) return;
  out.y[out.n] = y;
  out.x0[out.n] = x0;
  out.x1[out.n] = x1;
  out.n++;
}

static void eraseRunsDiff(const HandRuns &oldR, const HandRuns &newR,
                           Sliver &out) {
  out.n = 0;
  for (int16_t i = 0; i < oldR.n; i++) {
    int16_t y = oldR.ry[i], a = oldR.rx0[i], b = oldR.rx1[i];
    int16_t na = 1, nb = 0; // empty
    for (int16_t j = 0; j < newR.n; j++) {
      if (newR.ry[j] == y) {
        na = newR.rx0[j];
        nb = newR.rx1[j];
        break;
      }
    }
    if (na > nb) { // no new run on this row: whole old run vacated
      addSliverSeg(out, y, a, b);
    } else {
      if (a < na) addSliverSeg(out, y, a, (int16_t)(na - 1) < b ? (int16_t)(na - 1) : b);
      if (b > nb) addSliverSeg(out, y, (int16_t)(nb + 1) > a ? (int16_t)(nb + 1) : a, b);
    }
  }
}

static void drawHub() { gfxFillCircle(CX, CY, HUB_R, COLOR_HUB); }

// ---------------------------------------------------------------------
// Static face (drawn once)
// ---------------------------------------------------------------------
static const char *NUMERALS[12] = {"12", "1", "2", "3", "4",  "5",
                                    "6",  "7", "8", "9", "10", "11"};

static const uint8_t NUMERAL_SCALE = 2;

static void drawNumeral(int i) {
  int16_t x, y;
  polarToXY(i * 30.0f, NUMERAL_R, x, y);
  int16_t textW = strlen(NUMERALS[i]) * GLYPH_W * NUMERAL_SCALE;
  int16_t textH = GLYPH_H * NUMERAL_SCALE;
  gfxDrawNumber(x - textW / 2, y - textH / 2, NUMERALS[i], NUMERAL_SCALE,
                COLOR_NUMERAL, COLOR_BG);
}

// Answers "what colour is the static face at this pixel" - background, or
// a numeral's ink. The moving hands and the scrub restore exactly the
// pixels they vacate through this, which replaces the old scheme of
// repainting a whole ~470px numeral cell to fix the two or three pixels a
// passing hand actually damaged. In safe write mode, where each pixel
// costs real time, those blanket cell repaints (plus the full hand
// repaints they forced for stacking order) were what stretched frames to
// ~100ms during numeral crossings and made the hands visibly flicker.
//
// Ticks and the rim never need this: everything that moves stays inside
// r~147 and the ticks start at r=150.
static int16_t numCellX[12], numCellY[12], numCellW[12];
static bool numCellsReady = false;

static void initNumeralCells() {
  for (int i = 0; i < 12; i++) {
    int16_t x, y;
    polarToXY(i * 30.0f, NUMERAL_R, x, y);
    numCellW[i] = (int16_t)(strlen(NUMERALS[i]) * GLYPH_W * NUMERAL_SCALE);
    numCellX[i] = x - numCellW[i] / 2;
    numCellY[i] = y - (GLYPH_H * NUMERAL_SCALE) / 2;
  }
  numCellsReady = true;
}

static bool numeralInkAt(int16_t x, int16_t y) {
  if (!numCellsReady) initNumeralCells();
  for (int i = 0; i < 12; i++) {
    int16_t lx = x - numCellX[i];
    int16_t ly = y - numCellY[i];
    if (lx < 0 || ly < 0 || lx >= numCellW[i] ||
        ly >= GLYPH_H * NUMERAL_SCALE) {
      continue;
    }
    int16_t col = lx / NUMERAL_SCALE;
    int16_t row = ly / NUMERAL_SCALE;
    int16_t glyphCol = col % GLYPH_W;
    if (glyphCol >= 5 || row >= 7) return false; // inter-glyph spacing
    uint8_t digit = (uint8_t)(NUMERALS[i][col / GLYPH_W] - '0');
    return (DIGIT_FONT[digit][glyphCol] >> row) & 1;
  }
  return false;
}

// Puts one pixel back to what the static face has there.
static void restoreFacePixel(int16_t x, int16_t y) {
  panelDrawPixel(x, y, numeralInkAt(x, y) ? COLOR_NUMERAL : COLOR_BG);
}

static bool pixelInRuns(int16_t x, int16_t y, const HandRuns &r) {
  for (int16_t i = 0; i < r.n; i++) {
    if (r.ry[i] == y) return x >= r.rx0[i] && x <= r.rx1[i];
  }
  return false;
}

// Paints one vacated pixel with whatever should be visible there, going
// down the stack: second hand, hub, minute, hour, then the static face.
//
// This is the whole anti-flicker principle in one function. Restoring to
// background and then repainting whatever was damaged on top leaves those
// pixels dark for the rest of the frame - which in safe write mode is
// milliseconds, and is precisely what made the hands flicker where the
// second hand swept along them. Writing the final colour immediately
// means a pixel is never wrong even for one write, and it removes the
// need for damage flags and stacking-order repaints entirely.
static void restoreStackPixel(int16_t x, int16_t y, const int16_t *sx,
                               const int16_t *sy, int16_t sn) {
  if (inPixelList(x, y, sx, sy, sn)) {
    panelDrawPixel(x, y, COLOR_SEC_HAND);
    return;
  }
  int16_t dx = x - CX, dy = y - CY;
  if (dx * dx + dy * dy <= HUB_R * HUB_R) {
    panelDrawPixel(x, y, COLOR_HUB);
    return;
  }
  if (pixelInRuns(x, y, minRuns)) {
    panelDrawPixel(x, y, COLOR_MIN_HAND);
    return;
  }
  if (pixelInRuns(x, y, hourRuns)) {
    panelDrawPixel(x, y, COLOR_HOUR_HAND);
    return;
  }
  restoreFacePixel(x, y);
}

// Restores a sliver's pixels straight to their final colours.
static void restoreSliver(const Sliver &s, const int16_t *sx,
                           const int16_t *sy, int16_t sn) {
  for (int16_t i = 0; i < s.n; i++) {
    for (int16_t x = s.x0[i]; x <= s.x1[i]; x++) {
      restoreStackPixel(x, s.y[i], sx, sy, sn);
    }
  }
}

static void drawFace() {
  panelFillRect(0, 0, PANEL_W, PANEL_H, COLOR_BG);
  gfxDrawCircle(CX, CY, RIM_OUTER_R, COLOR_RIM);
  gfxDrawCircle(CX, CY, RIM_INNER_R, COLOR_RIM);

  for (int i = 0; i < 60; i++) {
    float angle = i * 6.0f;
    int16_t ox, oy, ix, iy;
    if (i % 5 == 0) {
      polarToXY(angle, HOUR_TICK_OUTER_R, ox, oy);
      polarToXY(angle, HOUR_TICK_INNER_R, ix, iy);
      drawThickLine(ix, iy, ox, oy, COLOR_TICK, 3);
    } else {
      polarToXY(angle, MIN_TICK_OUTER_R, ox, oy);
      polarToXY(angle, MIN_TICK_INNER_R, ix, iy);
      gfxDrawLine(ix, iy, ox, oy, COLOR_TICK);
    }
  }

  for (int i = 0; i < 12; i++) {
    drawNumeral(i);
  }
}

// ---------------------------------------------------------------------
// Time acquisition
// ---------------------------------------------------------------------

// Falls back to the firmware build time when NTP isn't available, so the
// clock still runs (it will simply drift from the wall clock and won't
// reflect the real date/time until the sketch is rebuilt or Wi-Fi is
// configured).
static void setFallbackTime() {
  static const char *MONTHS[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  // Apply the same time zone the NTP path would, so mktime() below reads
  // the build timestamp as local time and the display renders it back
  // unchanged. Without this the fallback clock would silently be
  // interpreted as UTC.
  setenv("TZ", TZ_INFO, 1);
  tzset();

  char monStr[4];
  struct tm t = {};
  int day, year, hour, min, sec;
  sscanf(__DATE__, "%3s %d %d", monStr, &day, &year);
  sscanf(__TIME__, "%d:%d:%d", &hour, &min, &sec);

  t.tm_mday = day;
  t.tm_year = year - 1900;
  t.tm_hour = hour;
  t.tm_min = min;
  t.tm_sec = sec;
  t.tm_mon = 0;
  for (int i = 0; i < 12; i++) {
    if (strncmp(monStr, MONTHS[i], 3) == 0) {
      t.tm_mon = i;
      break;
    }
  }

  time_t epoch = mktime(&t);
  struct timeval tv = {epoch, 0};
  settimeofday(&tv, nullptr);
  Serial.println("Using firmware build time as a fallback clock.");
}

// ---------------------------------------------------------------------
// Wi-Fi duty cycling
// ---------------------------------------------------------------------
// The radio stays OFF except during time syncs. Wi-Fi transmit bursts pull
// hard on the Super Mini's small 3V3 regulator, and the display shares
// that rail - the stray-pixel bursts and one outright panel latch-up both
// line up with radio activity. An analog clock needs the network for a few
// seconds every few hours, so the radio simply is not kept around.
// Between syncs the time free-runs on the crystal (~1-2s/day drift, reset
// at each sync).
static const uint32_t RESYNC_OK_INTERVAL_MS = 6UL * 3600UL * 1000UL;
static const uint32_t RESYNC_RETRY_MS = 15UL * 60UL * 1000UL;
static const uint32_t RESYNC_CONNECT_TIMEOUT_MS = 20000;
// How long the radio stays up after connecting so SNTP (restarted by
// configTzTime, which fires a request immediately) can complete.
static const uint32_t RESYNC_HOLD_MS = 15000;

static bool resyncArmed = false; // set once initial sync has run
static uint8_t resyncPhase = 0;  // 0 idle, 1 connecting, 2 sync window
static uint32_t resyncDueMs = 0;
static uint32_t resyncPhaseStartMs = 0;

static bool wifiConfigured() {
  return strcmp(WIFI_SSID, "YOUR_WIFI_SSID") != 0;
}

static void wifiOff() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

static void connectAndSyncTime() {
  if (!wifiConfigured()) {
    Serial.println("Wi-Fi not configured, skipping NTP sync.");
    setFallbackTime();
    return;
  }

  Serial.printf("Connecting to Wi-Fi \"%s\"", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  bool synced = false;
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi connect failed, using fallback time.");
    setFallbackTime();
  } else {
    Serial.println("Wi-Fi connected, syncing time via NTP...");
    configTzTime(TZ_INFO, NTP_SERVER_1, NTP_SERVER_2);

    struct tm t;
    if (getLocalTime(&t, 10000)) {
      Serial.println("Time synced.");
      synced = true;
    } else {
      Serial.println("NTP sync timed out, using fallback time.");
      setFallbackTime();
    }
  }

  wifiOff();
  Serial.println("Wi-Fi radio off until the next sync window.");
  resyncArmed = true;
  resyncPhase = 0;
  resyncDueMs = millis() + (synced ? RESYNC_OK_INTERVAL_MS : RESYNC_RETRY_MS);
}

// Non-blocking periodic re-sync, driven from loop() so the hands never
// freeze while the radio negotiates.
static void resyncTick() {
  if (!resyncArmed || !wifiConfigured()) return;
  uint32_t now = millis();
  switch (resyncPhase) {
    case 0:
      if ((int32_t)(now - resyncDueMs) < 0) return;
      Serial.println("Time re-sync: radio on, connecting...");
      WiFi.mode(WIFI_STA);
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      resyncPhase = 1;
      resyncPhaseStartMs = now;
      break;
    case 1:
      if (WiFi.status() == WL_CONNECTED) {
        configTzTime(TZ_INFO, NTP_SERVER_1, NTP_SERVER_2);
        resyncPhase = 2;
        resyncPhaseStartMs = now;
      } else if (now - resyncPhaseStartMs > RESYNC_CONNECT_TIMEOUT_MS) {
        Serial.println("Time re-sync: connect failed, retrying later.");
        wifiOff();
        resyncPhase = 0;
        resyncDueMs = now + RESYNC_RETRY_MS;
      }
      break;
    case 2:
      if (now - resyncPhaseStartMs > RESYNC_HOLD_MS) {
        Serial.println("Time re-sync done, radio off.");
        wifiOff();
        resyncPhase = 0;
        resyncDueMs = now + RESYNC_OK_INTERVAL_MS;
      }
      break;
  }
}

// ---------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------

// Reads the wall clock with sub-second resolution. secOfMinute comes back
// as tm_sec plus the fraction elapsed within that second (0.0 .. 60.0),
// or as a whole number when SMOOTH_SECONDS is off.
static void readClock(struct tm &t, float &secOfMinute) {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  localtime_r(&tv.tv_sec, &t);
  secOfMinute = t.tm_sec;
  if (SMOOTH_SECONDS) {
    secOfMinute += tv.tv_usec / 1000000.0f;
  }
}

// Redraws only what moved. Erasing is the expensive, damaging operation,
// so it is done sparingly: the second hand every frame, the other two
// only when they have actually shifted a pixel.
static void renderHands(const Hand &newHour, const Hand &newMin,
                         const Hand &newSec) {
  bool hourMoved = !sameHand(newHour, hourHand);
  bool minMoved = !sameHand(newMin, minHand);

  // One SPI transaction for the whole frame rather than one per primitive.
  panelBeginBatch();

  collectLine(newSec.tx, newSec.ty, newSec.x, newSec.y, secNewX, secNewY,
              secNewN);

  hourHand = newHour;
  minHand = newMin;
  secHand = newSec;

  // Thick hands first, so hourRuns/minRuns describe the final positions
  // before anything consults them to decide a pixel's colour.
  //
  // A step restores exactly the vacated pixels (old runs minus new) and
  // then paints the hand whole. Painting is additive - it never blanks
  // anything - so a full repaint costs time but cannot flicker, and it
  // gives the thick hands the same dropped-write healing the second hand
  // gets. The vacated sliver is re-restored for ERASE_RETRIES further
  // frames; it is disjoint from the hand's own pixels by construction, so
  // those retries darken nothing.
  static HandRuns scratchRuns;
  if (hourMoved) {
    handQuadRuns(newHour, HOUR_HAND_W, scratchRuns);
    eraseRunsDiff(hourRuns, scratchRuns, hourSliver);
    hourRuns = scratchRuns;
    hourSliverLeft = ERASE_RETRIES;
  }
  if (minMoved) {
    handQuadRuns(newMin, MIN_HAND_W, scratchRuns);
    eraseRunsDiff(minRuns, scratchRuns, minSliver);
    minRuns = scratchRuns;
    minSliverLeft = ERASE_RETRIES;
  }
  if (hourMoved || hourSliverLeft > 0) {
    restoreSliver(hourSliver, secNewX, secNewY, secNewN);
    if (!hourMoved) hourSliverLeft--;
  }
  if (minMoved || minSliverLeft > 0) {
    restoreSliver(minSliver, secNewX, secNewY, secNewN);
    if (!minMoved) minSliverLeft--;
  }
  // Stacking: the minute hand sits above the hour hand, and their quads
  // always overlap near the hub - so painting the hour hand puts hour ink
  // over minute pixels and the minute hand has to follow it. Both are
  // additive, so this costs writes but can never blank anything.
  if (hourMoved) paintRuns(hourRuns, COLOR_HOUR_HAND);
  if (hourMoved || minMoved) paintRuns(minRuns, COLOR_MIN_HAND);

  // The second hand's trailing edge. Each vacated pixel goes straight to
  // its final colour - numeral ink, the minute or hour hand it was lying
  // on, the hub - never to background followed by a repaint. That is what
  // stops the thick hands flickering wherever the sweep grazes them.
  int16_t thisEraseN = 0;
  for (int16_t i = 0; i < secOldN; i++) {
    int16_t px = secOldX[i], py = secOldY[i];
    if (inPixelList(px, py, secNewX, secNewY, secNewN)) continue;
    restoreStackPixel(px, py, secNewX, secNewY, secNewN);
    thisEraseX[thisEraseN] = px;
    thisEraseY[thisEraseN] = py;
    thisEraseN++;
  }

  // Restore the last few frames' discards again - insurance against the
  // panel dropping a write, since a dropped restore is the one that
  // leaves a permanent mark.
  for (int8_t g = 0; g < ERASE_RETRIES; g++) {
    for (int16_t i = 0; i < eraseGenN[g]; i++) {
      int16_t px = eraseGenX[g][i], py = eraseGenY[g][i];
      if (inPixelList(px, py, secNewX, secNewY, secNewN)) continue;
      restoreStackPixel(px, py, secNewX, secNewY, secNewN);
    }
  }

  drawHub();
  // The whole second hand, every frame - repainting every pixel is what
  // lets a dropped write heal instead of becoming permanent.
  drawPixelList(secNewX, secNewY, secNewN, COLOR_SEC_HAND);

  for (int8_t g = ERASE_RETRIES - 1; g > 0; g--) {
    memcpy(eraseGenX[g], eraseGenX[g - 1], eraseGenN[g - 1] * sizeof(int16_t));
    memcpy(eraseGenY[g], eraseGenY[g - 1], eraseGenN[g - 1] * sizeof(int16_t));
    eraseGenN[g] = eraseGenN[g - 1];
  }
  memcpy(eraseGenX[0], thisEraseX, thisEraseN * sizeof(int16_t));
  memcpy(eraseGenY[0], thisEraseY, thisEraseN * sizeof(int16_t));
  eraseGenN[0] = thisEraseN;

  memcpy(secOldX, secNewX, secNewN * sizeof(int16_t));
  memcpy(secOldY, secNewY, secNewN * sizeof(int16_t));
  secOldN = secNewN;

  panelEndBatch();
}

// ---------------------------------------------------------------------
// Background scrub
// ---------------------------------------------------------------------
// The dial's self-healing backstop. The panel drops writes in bursts, and
// any scheme that erases a pixel a fixed number of times can be beaten by
// a burst longer than its window - the 3-attempt erase retry above
// reduces strays but cannot bound their lifetime. This does: once a
// second, a hand-shaped wedge at a slowly advancing bearing is erased and
// the things it may have hit are repainted, sweeping the full circle
// every 6 minutes like a radar. Any stray anywhere on the hand's reach is
// gone within a revolution or two, no matter how it got there.
//
// The scrub is skipped while its bearing is near the second hand's (both
// ends), so it never blanks a visible stretch of the live hand; a skipped
// bearing is simply caught on the next revolution, by which time the hand
// has moved on. Everything it
// erases is restored or repainted in the same batch, so on a healthy
// panel the scrub is pixel-for-pixel invisible - the framebuffer tests
// assert exactly that.
static const uint32_t SCRUB_PERIOD_MS = 1000;
static const float SCRUB_STEP_DEG = 1.0f;  // 2.5px arc at the tip, less
                                            // than the 3px scrub width, so
                                            // revolutions leave no gaps
static const uint8_t SCRUB_W = 3;
static const float SCRUB_KEEPOUT_DEG = 25.0f;

static float scrubAngle = 0.0f;
static uint32_t lastScrubMs = 0;

static void scrubTick() {
  uint32_t now = millis();
  if (now - lastScrubMs < SCRUB_PERIOD_MS) return;

  // Keep clear of the live second hand - shaft side and tail side both.
  //
  // Crucially, a skipped bearing is NOT advanced past: the scrub waits on
  // it (re-checking every pass, at most ~8s until the hand moves clear)
  // and only moves on once it has actually been cleaned. The first
  // version advanced regardless, and that was a bug with a signature: the
  // scrub's revolution took exactly 360s and the hand's exactly 60s, so
  // each bearing always met the hand at the same angle - and the bearings
  // whose meeting fell inside this keep-out were skipped on every
  // revolution forever. Strays accumulated in fixed ~10-degree bands
  // spaced 36 degrees apart while the rest of the dial stayed clean.
  // Waiting instead of skipping makes the schedule depend on the hand's
  // actual position, which breaks the lock-step for good.
  float d = angleDelta(scrubAngle, secHand.angle);
  if (d < SCRUB_KEEPOUT_DEG || d > 180.0f - SCRUB_KEEPOUT_DEG) return;

  lastScrubMs = now;
  float bearing = scrubAngle;
  scrubAngle += SCRUB_STEP_DEG;
  if (scrubAngle >= 360.0f) scrubAngle -= 360.0f;

  Hand v;
  v.angle = bearing;
  polarToXY(bearing, SEC_HAND_LEN, v.x, v.y);
  polarToXY(bearing + 180.0f, SEC_HAND_TAIL, v.tx, v.ty);

  panelBeginBatch();

  // Restore the face across the wedge pixel by pixel - numeral ink comes
  // back through restoreFacePixel, so no cell repaints - then repaint
  // only what the wedge could actually have crossed. The hour and minute
  // hands are hit only when the wedge bearing runs close to them (wide
  // margins, because near the centre a hand subtends a large angle); the
  // second hand is protected by the keep-out, and its only possible
  // overlap with the wedge is inside the hub, which is repainted here in
  // the same colour.
  // Each wedge pixel is written straight to its final colour, so nothing
  // is ever transiently dark and no repaint or damage flag is needed. An
  // earlier version blanked the wedge and then repainted whichever hands
  // an angle threshold guessed it had hit - wrong near the hub, where
  // every hand overlaps whatever the bearing, which left their inner
  // pixels dark until something else happened to repaint them.
  static HandRuns wedge;
  handQuadRuns(v, SCRUB_W, wedge);
  for (int16_t i = 0; i < wedge.n; i++) {
    for (int16_t x = wedge.rx0[i]; x <= wedge.rx1[i]; x++) {
      restoreStackPixel(x, wedge.ry[i], secOldX, secOldY, secOldN);
    }
  }

  panelEndBatch();
}

// ---------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  // On the C3's native USB, prints made before the host opens the port
  // are lost. A short bounded wait keeps the boot breadcrumbs visible in
  // a monitor that is already attached, without stalling a standalone
  // clock that has no USB plugged in.
  uint32_t serialWait = millis();
  while (!Serial && millis() - serialWait < 2000) {
    delay(10);
  }

  // Boot breadcrumbs. Each stage announces itself so a dead display can
  // be localised from the serial monitor alone: the last line printed
  // names the stage that hung or crashed, and if all of them appear the
  // sketch is running fine and the fault is on the panel side (wiring,
  // reset, init) rather than in this code.
  Serial.println("[boot] sketch start");

  panelInit();
  Serial.println("[boot] panel init done");

  // Boot self-test: a moment of solid red before the face. This is the
  // same known-good result the vendor demo produces (a red screen), sent
  // through this sketch's own adapter - so it splits the world cleanly.
  // Red appears: panel, wiring and init are fine, and whatever else is
  // wrong is in this sketch. No red: the commands are not reaching the
  // glass - reseat the wiring, power-cycle the panel. The clock's own
  // face is a poor test for this because its background is near-black,
  // indistinguishable from a dead panel.
  panelBeginBatch();
  panelFillRect(0, 0, PANEL_W, PANEL_H, RGB565(255, 0, 0));
  panelEndBatch();
  Serial.println("[boot] RED self-test fill sent - screen should be red now");
  delay(1500);

  panelBeginBatch();
  panelFillRect(0, 0, PANEL_W, PANEL_H, COLOR_BG);
  panelEndBatch();
  Serial.println("[boot] background filled");

  connectAndSyncTime();

  panelBeginBatch();
  drawFace();

  struct tm t;
  float secOfMinute;
  readClock(t, secOfMinute);
  computeHands(t, secOfMinute, hourHand, minHand, secHand);
  handQuadRuns(hourHand, HOUR_HAND_W, hourRuns);
  handQuadRuns(minHand, MIN_HAND_W, minRuns);
  paintRuns(hourRuns, COLOR_HOUR_HAND);
  paintRuns(minRuns, COLOR_MIN_HAND);
  drawHub();
  paintSecondHandFresh();
  panelEndBatch();
  Serial.println("[boot] face drawn, clock running");
}

void loop() {
  resyncTick();
  scrubTick();

  struct tm t;
  float secOfMinute;
  readClock(t, secOfMinute);

  Hand newHour, newMin, newSec;
  computeHands(t, secOfMinute, newHour, newMin, newSec);

  // The sweep is quantized by the display, not by a timer: at the second
  // hand's length one pixel of tip travel takes about 66ms, so redrawing
  // only when a rounded coordinate actually changes gives the smoothest
  // motion the panel can show without any wasted frames.
  if (sameHand(newSec, secHand) && sameHand(newMin, minHand) &&
      sameHand(newHour, hourHand)) {
    delay(5);
    return;
  }

  renderHands(newHour, newMin, newSec);
}

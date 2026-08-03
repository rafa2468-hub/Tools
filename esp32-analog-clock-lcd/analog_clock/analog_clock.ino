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

static void panelDrawPixel(int16_t x, int16_t y, uint16_t color) {
  if (x < 0 || y < 0 || x >= PANEL_W || y >= PANEL_H) return;
  panelOpenWindow(x, y, x, y);
  SPI.transfer(color >> 8);
  SPI.transfer(color & 0xFF);
  panelCloseWindow();
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
static const uint8_t SEC_HAND_W = 1;

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

static void eraseHand(const Hand &h, uint8_t thickness) {
  drawThickLine(h.tx, h.ty, h.x, h.y, COLOR_BG, thickness);
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

// Perpendicular distance from a point to a hand's segment, used to work
// out whether erasing a pixel damaged one of the other hands.
static bool pixelTouchesHand(int16_t px, int16_t py, const Hand &h,
                              float tol) {
  float ax = h.tx, ay = h.ty;
  float dx = (float)h.x - ax, dy = (float)h.y - ay;
  float len2 = dx * dx + dy * dy;
  float t = 0.0f;
  if (len2 > 0.0f) {
    t = ((px - ax) * dx + (py - ay) * dy) / len2;
    if (t < 0.0f) t = 0.0f;
    else if (t > 1.0f) t = 1.0f;
  }
  float ex = px - (ax + t * dx), ey = py - (ay + t * dy);
  return ex * ex + ey * ey <= tol * tol;
}

static void paintHand(const Hand &h, uint16_t color, uint8_t thickness) {
  drawThickLine(h.tx, h.ty, h.x, h.y, color, thickness);
}

static void drawHub() { gfxFillCircle(CX, CY, 6, COLOR_HUB); }

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

// The second and minute hands are long enough to reach into the ring of
// numerals, so erasing one punches a hole in whichever numeral it was
// lying across. Repaint just that numeral. The widest label ("12") spans
// about 24px at NUMERAL_R, i.e. roughly +/-6 degrees, so a 10 degree
// guard band covers it with room to spare - and at 30 degree spacing it
// still matches at most one numeral per call.
static const float NUMERAL_GUARD_DEG = 10.0f;

static void repairNumeralsNear(float angleDeg) {
  for (int i = 0; i < 12; i++) {
    if (angleDelta(angleDeg, i * 30.0f) < NUMERAL_GUARD_DEG) {
      drawNumeral(i);
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
  float oldSecAngle = secHand.angle;

  // One SPI transaction for the whole frame rather than one per primitive.
  panelBeginBatch();

  collectLine(newSec.tx, newSec.ty, newSec.x, newSec.y, secNewX, secNewY,
              secNewN);

  // Erase the old position and draw the new one in one interleaved pass.
  // Every pixel of the hand is rewritten every frame, so a write the panel
  // drops is corrected on the next frame instead of becoming a permanent
  // speck - and because each pixel's redraw follows its erase immediately,
  // nothing is dark for more than a single write.
  //
  // Damage is assessed only for pixels the new position does *not* reuse,
  // since those are the ones left showing background. Rather than guess at
  // which angles overlap - an angle threshold was tried here and was wrong
  // near the centre, where a 3px hand subtends ~19 degrees at r=6 - each
  // such pixel is tested against what it could have hit. That is exact,
  // and cheap, because there are only a handful of them.
  bool repairHour = false, repairMin = false, repairNums = false;
  int16_t thisEraseN = 0;
  for (int16_t i = 0; i < secOldN; i++) {
    int16_t px = secOldX[i], py = secOldY[i];
    if (inPixelList(px, py, secNewX, secNewY, secNewN)) continue;
    panelDrawPixel(px, py, COLOR_BG);
    thisEraseX[thisEraseN] = px;
    thisEraseY[thisEraseN] = py;
    thisEraseN++;
    if (pixelTouchesHand(px, py, hourHand, HOUR_HAND_W / 2.0f + 1.0f))
      repairHour = true;
    if (pixelTouchesHand(px, py, minHand, MIN_HAND_W / 2.0f + 1.0f))
      repairMin = true;
    int16_t dx = px - CX, dy = py - CY;
    if (dx * dx + dy * dy >= (NUMERAL_R - 16) * (NUMERAL_R - 16))
      repairNums = true;
  }

  // Erase the last few frames' discards again. They are already
  // background, so this is invisible - it exists because the panel
  // occasionally drops a write, and a dropped *erase* is the one that
  // leaves a mark. The hand's own pixels are repainted every frame and so
  // heal themselves; discarded pixels are written once and never revisited,
  // which is how single dropped writes accumulated into comet-trails along
  // the hand's path. Retrying each discard on the next few frames makes a
  // stray need several consecutive drops in the same place.
  for (int8_t g = 0; g < ERASE_RETRIES; g++) {
    for (int16_t i = 0; i < eraseGenN[g]; i++) {
      int16_t px = eraseGenX[g][i], py = eraseGenY[g][i];
      // Guard against the clock stepping backwards (an NTP correction can
      // do it) and the hand revisiting a pixel we are about to blank.
      if (inPixelList(px, py, secNewX, secNewY, secNewN)) continue;
      panelDrawPixel(px, py, COLOR_BG);
      if (pixelTouchesHand(px, py, hourHand, HOUR_HAND_W / 2.0f + 1.0f))
        repairHour = true;
      if (pixelTouchesHand(px, py, minHand, MIN_HAND_W / 2.0f + 1.0f))
        repairMin = true;
      int16_t dx = px - CX, dy = py - CY;
      if (dx * dx + dy * dy >= (NUMERAL_R - 16) * (NUMERAL_R - 16))
        repairNums = true;
    }
  }

  if (hourMoved) {
    eraseHand(hourHand, HOUR_HAND_W);
    repairHour = true;
  }
  if (minMoved) {
    eraseHand(minHand, MIN_HAND_W);
    // At the stock MIN_HAND_LEN the tip stops a pixel or two short of the
    // numerals' ink, so this repair is a no-op - but it is what keeps a
    // lengthened minute hand from scraping the numerals, which is the
    // first thing anyone reaching for a different look will change.
    repairNumeralsNear(minHand.angle);
    repairMin = true;
  }

  hourHand = newHour;
  minHand = newMin;
  secHand = newSec;

  // Repainting has to preserve the stacking order a full repaint would
  // produce - numerals, then hour, then minute, then hub, then the second
  // hand. Restoring only the damaged item would put it on top of things
  // that should cover it: repaint the hour hand alone where it crosses the
  // minute hand and the minute hand ends up underneath, which is a real
  // pixel difference. So any repair at all replays the whole stack. It is
  // all additive drawing - no erasing - so it costs time, not flicker.
  if (repairNums) repairNumeralsNear(oldSecAngle);
  if (repairNums || repairHour || repairMin) {
    paintHand(hourHand, COLOR_HOUR_HAND, HOUR_HAND_W);
    paintHand(minHand, COLOR_MIN_HAND, MIN_HAND_W);
  }
  drawHub(); // cheap, and in ticking mode the step is big enough to
             // reach the hub, so this is not always redundant

  // The whole hand, every frame - not just the newly-uncovered pixels. A
  // repair above may have painted over pixels the two positions share, and
  // repainting every pixel is also what lets a dropped write heal instead
  // of becoming permanent.
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
// has moved on. Everything it erases is repainted in the same batch, in
// stacking order, so on a healthy panel the scrub is pixel-for-pixel
// invisible - the framebuffer tests assert exactly that.
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
  eraseHand(v, SCRUB_W);
  // Replay the stack over the scrubbed wedge, same order as a full
  // repaint: numerals, hour, minute, hub, second hand.
  repairNumeralsNear(bearing);
  paintHand(hourHand, COLOR_HOUR_HAND, HOUR_HAND_W);
  paintHand(minHand, COLOR_MIN_HAND, MIN_HAND_W);
  drawHub();
  drawPixelList(secOldX, secOldY, secOldN, COLOR_SEC_HAND);
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
  paintHand(hourHand, COLOR_HOUR_HAND, HOUR_HAND_W);
  paintHand(minHand, COLOR_MIN_HAND, MIN_HAND_W);
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

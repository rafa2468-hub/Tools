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

// SPI clock for pixel pushing. The GC9B72 is happy well above this; back
// it off if the wiring is long or on breadboard jumpers.
static const uint32_t PANEL_SPI_HZ = 40000000;
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
// Hand state, tracked so each redraw only touches what changed
// ---------------------------------------------------------------------
//
// This has to be declared ahead of every function, not just ahead of the
// ones that use it. The Arduino IDE generates prototypes for the sketch
// and injects them immediately before the *first* function definition in
// the file, so any type named in a signature must already exist at that
// point. With the struct further down, the IDE emits a wall of
// "'Hand' has not been declared". (PlatformIO compiles the .ino as plain
// C++ and never does this, so the build stays green there either way -
// which is exactly why it is worth a comment.)
struct Hand {
  int16_t x, y;   // tip
  int16_t tx, ty; // tail (the center, except for the second hand)
  float angle;    // bearing of the tip, degrees clockwise from 12
};
static Hand hourHand, minHand, secHand;

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

static void connectAndSyncTime() {
  if (strcmp(WIFI_SSID, "YOUR_WIFI_SSID") == 0) {
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

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi connect failed, using fallback time.");
    setFallbackTime();
    return;
  }

  Serial.println("Wi-Fi connected, syncing time via NTP...");
  configTzTime(TZ_INFO, NTP_SERVER_1, NTP_SERVER_2);

  struct tm t;
  if (getLocalTime(&t, 10000)) {
    Serial.println("Time synced.");
  } else {
    Serial.println("NTP sync timed out, using fallback time.");
    setFallbackTime();
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

  // One SPI transaction for the whole frame. Without this LovyanGFX opens
  // and closes a transaction around every individual drawLine, and a
  // frame is made of dozens of them.
  panelBeginBatch();

  eraseHand(secHand, SEC_HAND_W);
  repairNumeralsNear(secHand.angle);

  if (hourMoved) {
    eraseHand(hourHand, HOUR_HAND_W);
  }
  if (minMoved) {
    eraseHand(minHand, MIN_HAND_W);
    // At the stock MIN_HAND_LEN the tip stops a pixel or two short of the
    // numerals' ink, so this repair is a no-op - but it is what keeps a
    // lengthened minute hand from scraping the numerals, which is the
    // first thing anyone reaching for a different look will change.
    repairNumeralsNear(minHand.angle);
  }

  hourHand = newHour;
  minHand = newMin;
  secHand = newSec;

  // The second hand's erase runs from its tail through the center out to
  // its tip, so it can nick the other two hands anywhere along their
  // length - and near the center it does so even at large angular
  // separations, because a 3px-wide hand subtends ~19 degrees at r=6.
  // Rather than guard that with an angle threshold (which is fiddly to
  // get right and fails silently when it's wrong), just repaint both
  // unconditionally: drawing is idempotent and costs under a thousand
  // pixels a frame.
  paintHand(hourHand, COLOR_HOUR_HAND, HOUR_HAND_W);
  paintHand(minHand, COLOR_MIN_HAND, MIN_HAND_W);
  drawHub();
  paintHand(secHand, COLOR_SEC_HAND, SEC_HAND_W);

  panelEndBatch();
}

// ---------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------

void setup() {
  Serial.begin(115200);

  // Backlight is handled by the panel definition (LovyanGFX drives BL
  // itself when the vendor file configures a Light_PWM block; on modules
  // where BL is tied high it is simply always on).
  panelInit();
  panelBeginBatch();
  panelFillRect(0, 0, PANEL_W, PANEL_H, COLOR_BG);
  panelEndBatch();

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
  paintHand(secHand, COLOR_SEC_HAND, SEC_HAND_W);
  panelEndBatch();
}

void loop() {
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

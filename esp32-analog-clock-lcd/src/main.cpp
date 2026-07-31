// Analog clock for a 2.1" round 360x360 GC9B72 SPI TFT, driven by an
// ESP32-C3 Super Mini.
//
// The GC9B72 is a GalaxyCore round-panel controller with no dedicated
// driver in the Arduino_GFX library at the time this was written. It is
// register-compatible with GalaxyCore's other 360x360 round part, GC9C01,
// so this sketch drives the panel through the Arduino_GC9C01 class. If
// your panel shows a mirrored, rotated, or offset image, see the
// troubleshooting notes in README.md.
//
// Time is obtained over Wi-Fi via NTP (the ESP32-C3 has no RTC of its
// own). If Wi-Fi is not configured or the connection attempt fails, the
// clock falls back to the firmware's compile timestamp and free-runs from
// there so it still displays something useful.

#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <time.h>
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

// ---------------------------------------------------------------------
// Display wiring - ESP32-C3 Super Mini
// ---------------------------------------------------------------------
#define TFT_SCK 4
#define TFT_MOSI 6
#define TFT_MISO -1 // display is write-only, no MISO
#define TFT_CS 7
#define TFT_DC 2    // see the strapping-pin note below
#define TFT_RST 3

// Backlight enable (active HIGH). Set to -1 if the module's BLK pin is
// tied straight to 3V3 (always on) or left unconnected; set it to a GPIO
// number to control brightness/blanking from software.
#define TFT_BL -1

// Note on GPIO2: it is one of the ESP32-C3's strapping pins, sampled at
// reset to select the boot mode, and must not be held LOW at that moment.
// A TFT's DC line is a high-impedance input, so in practice it does not
// disturb the strap and the board boots normally. If this board ever
// fails to boot with the display attached, that is the first thing to
// suspect - move DC to a non-strapping GPIO (e.g. 1, 5 or 10).

// ---------------------------------------------------------------------
// Display bus / driver
// ---------------------------------------------------------------------
Arduino_DataBus *bus = new Arduino_HWSPI(
    TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, TFT_MISO);
Arduino_GFX *gfx = new Arduino_GC9C01(bus, TFT_RST, /* rotation */ 0,
                                       /* IPS */ true);

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
// Geometry helpers
// ---------------------------------------------------------------------

// angleDeg: 0 = 12 o'clock, increasing clockwise.
static void polarToXY(float angleDeg, int16_t radius, int16_t &x,
                       int16_t &y) {
  float rad = angleDeg * DEG_TO_RAD;
  x = CX + (int16_t)lroundf(radius * sinf(rad));
  y = CY - (int16_t)lroundf(radius * cosf(rad));
}

static void drawThickLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                           uint16_t color, uint8_t thickness) {
  float dx = x1 - x0;
  float dy = y1 - y0;
  float len = sqrtf(dx * dx + dy * dy);
  if (len < 1.0f) {
    gfx->drawPixel(x0, y0, color);
    return;
  }
  float ox = -dy / len;
  float oy = dx / len;
  int8_t half = thickness / 2;
  for (int8_t i = -half; i <= half; i++) {
    int16_t ex = (int16_t)lroundf(ox * i);
    int16_t ey = (int16_t)lroundf(oy * i);
    gfx->drawLine(x0 + ex, y0 + ey, x1 + ex, y1 + ey, color);
  }
}

// ---------------------------------------------------------------------
// Hand state, tracked so each redraw only touches what changed
// ---------------------------------------------------------------------
struct Hand {
  int16_t x, y;   // current tip
  int16_t tx, ty; // current tail (only used by the second hand)
};
static Hand hourHand, minHand, secHand;

static void computeHands(const struct tm &t, Hand &hour, Hand &minute,
                          Hand &sec) {
  float secAngle = t.tm_sec * 6.0f;
  float minAngle = t.tm_min * 6.0f + t.tm_sec * 0.1f;
  float hourAngle = (t.tm_hour % 12) * 30.0f + t.tm_min * 0.5f;

  polarToXY(hourAngle, HOUR_HAND_LEN, hour.x, hour.y);
  hour.tx = CX;
  hour.ty = CY;

  polarToXY(minAngle, MIN_HAND_LEN, minute.x, minute.y);
  minute.tx = CX;
  minute.ty = CY;

  polarToXY(secAngle, SEC_HAND_LEN, sec.x, sec.y);
  polarToXY(secAngle + 180.0f, SEC_HAND_TAIL, sec.tx, sec.ty);
}

static void eraseHand(const Hand &h, uint8_t thickness) {
  drawThickLine(h.tx, h.ty, h.x, h.y, COLOR_BG, thickness);
}

static void drawHand(const Hand &h, uint16_t color, uint8_t thickness) {
  drawThickLine(h.tx, h.ty, h.x, h.y, color, thickness);
}

static void drawHub() { gfx->fillCircle(CX, CY, 6, COLOR_HUB); }

// ---------------------------------------------------------------------
// Static face (drawn once)
// ---------------------------------------------------------------------
static const char *NUMERALS[12] = {"12", "1", "2", "3", "4",  "5",
                                    "6",  "7", "8", "9", "10", "11"};

static void drawFace() {
  gfx->fillScreen(COLOR_BG);
  gfx->drawCircle(CX, CY, RIM_OUTER_R, COLOR_RIM);
  gfx->drawCircle(CX, CY, RIM_INNER_R, COLOR_RIM);

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
      gfx->drawLine(ix, iy, ox, oy, COLOR_TICK);
    }
  }

  gfx->setTextColor(COLOR_NUMERAL, COLOR_BG);
  gfx->setTextSize(2);
  for (int i = 0; i < 12; i++) {
    int16_t x, y;
    polarToXY(i * 30.0f, NUMERAL_R, x, y);
    int16_t textW = strlen(NUMERALS[i]) * 12; // 6px * textSize(2)
    gfx->setCursor(x - textW / 2, y - 8);
    gfx->print(NUMERALS[i]);
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
// Arduino entry points
// ---------------------------------------------------------------------
static int8_t lastSecDrawn = -1;

void setup() {
  Serial.begin(115200);

#if TFT_BL >= 0
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
#endif

  gfx->begin();
  gfx->fillScreen(COLOR_BG);

  connectAndSyncTime();

  drawFace();

  struct tm t;
  getLocalTime(&t, 1000);
  computeHands(t, hourHand, minHand, secHand);
  drawHand(hourHand, COLOR_HOUR_HAND, HOUR_HAND_W);
  drawHand(minHand, COLOR_MIN_HAND, MIN_HAND_W);
  drawHand(secHand, COLOR_SEC_HAND, SEC_HAND_W);
  drawHub();
  lastSecDrawn = t.tm_sec;
}

void loop() {
  struct tm t;
  if (!getLocalTime(&t, 200)) {
    delay(50);
    return;
  }
  if (t.tm_sec == lastSecDrawn) {
    delay(20);
    return;
  }
  lastSecDrawn = t.tm_sec;

  Hand newHour, newMin, newSec;
  computeHands(t, newHour, newMin, newSec);

  eraseHand(hourHand, HOUR_HAND_W);
  eraseHand(minHand, MIN_HAND_W);
  eraseHand(secHand, SEC_HAND_W);

  hourHand = newHour;
  minHand = newMin;
  secHand = newSec;

  drawHand(hourHand, COLOR_HOUR_HAND, HOUR_HAND_W);
  drawHand(minHand, COLOR_MIN_HAND, MIN_HAND_W);
  drawHand(secHand, COLOR_SEC_HAND, SEC_HAND_W);
  drawHub();
}

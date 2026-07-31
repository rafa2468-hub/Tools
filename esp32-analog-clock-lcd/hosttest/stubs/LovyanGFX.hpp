// Host-side stubs standing in for Arduino + LovyanGFX, so the clock's
// geometry and redraw logic can be exercised without hardware.
//
// Only the drawing calls the sketch actually uses are modelled, with
// LovyanGFX's signatures. The panel definition itself is stubbed
// separately in LGFX_GC9B72.hpp.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

#define DEG_TO_RAD 0.017453292519943295f
#define GFX_NOT_DEFINED -1
#define OUTPUT 1
#define HIGH 1

// ---- painted-surface model: we keep a real 360x360 framebuffer so tests
// can assert on what the panel would actually be showing.
static const int FB_W = 360, FB_H = 360;
extern uint16_t g_fb[FB_W * FB_H];
extern long g_pixelWrites;

inline void fbSet(int x, int y, uint16_t c) {
  g_pixelWrites++;
  if (x < 0 || y < 0 || x >= FB_W || y >= FB_H) return;
  g_fb[y * FB_W + x] = c;
}

namespace lgfx {
class LGFX_Device {
  int16_t cx = 0, cy = 0;
  uint16_t fg = 0xFFFF, bg = 0;
  uint8_t tsize = 1;
  int writeDepth = 0;

public:
  bool init() { return true; }
  void setRotation(uint8_t) {}
  // LovyanGFX reference-counts these, so nesting is legal.
  void startWrite() { writeDepth++; }
  void endWrite() {
    if (writeDepth > 0) writeDepth--;
  }
  int transactionDepth() const { return writeDepth; }
  void fillScreen(uint16_t c) {
    for (int i = 0; i < FB_W * FB_H; i++) g_fb[i] = c;
  }
  void drawPixel(int16_t x, int16_t y, uint16_t c) { fbSet(x, y, c); }

  // Bresenham, matching Adafruit_GFX/Arduino_GFX behaviour.
  void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t c) {
    bool steep = abs(y1 - y0) > abs(x1 - x0);
    if (steep) { std::swap(x0, y0); std::swap(x1, y1); }
    if (x0 > x1) { std::swap(x0, x1); std::swap(y0, y1); }
    int16_t dx = x1 - x0, dy = abs(y1 - y0);
    int16_t err = dx / 2, ystep = (y0 < y1) ? 1 : -1;
    for (; x0 <= x1; x0++) {
      if (steep) fbSet(y0, x0, c); else fbSet(x0, y0, c);
      err -= dy;
      if (err < 0) { y0 += ystep; err += dx; }
    }
  }
  void drawCircle(int16_t x0, int16_t y0, int16_t r, uint16_t c) {
    int16_t f = 1 - r, ddF_x = 1, ddF_y = -2 * r, x = 0, y = r;
    fbSet(x0, y0 + r, c); fbSet(x0, y0 - r, c);
    fbSet(x0 + r, y0, c); fbSet(x0 - r, y0, c);
    while (x < y) {
      if (f >= 0) { y--; ddF_y += 2; f += ddF_y; }
      x++; ddF_x += 2; f += ddF_x;
      fbSet(x0 + x, y0 + y, c); fbSet(x0 - x, y0 + y, c);
      fbSet(x0 + x, y0 - y, c); fbSet(x0 - x, y0 - y, c);
      fbSet(x0 + y, y0 + x, c); fbSet(x0 - y, y0 + x, c);
      fbSet(x0 + y, y0 - x, c); fbSet(x0 - y, y0 - x, c);
    }
  }
  void fillCircle(int16_t x0, int16_t y0, int16_t r, uint16_t c) {
    for (int16_t dy = -r; dy <= r; dy++)
      for (int16_t dx = -r; dx <= r; dx++)
        if (dx * dx + dy * dy <= r * r) fbSet(x0 + dx, y0 + dy, c);
  }
  void setTextColor(uint16_t f, uint16_t b) { fg = f; bg = b; }
  void setTextSize(uint8_t s) { tsize = s; }
  void setCursor(int16_t x, int16_t y) { cx = x; cy = y; }
  // Approximates the classic 6x8 font cell: fills the whole cell with bg
  // (as setTextColor with a bg does) and marks glyph pixels with fg.
  void print(const char *s) {
    for (const char *p = s; *p; p++) {
      for (int gy = 0; gy < 8 * tsize; gy++)
        for (int gx = 0; gx < 6 * tsize; gx++) {
          bool ink = (gx >= tsize && gx < 5 * tsize && gy >= tsize &&
                      gy < 7 * tsize);
          fbSet(cx + gx, cy + gy, ink ? fg : bg);
        }
      cx += 6 * tsize;
    }
  }
};
} // namespace lgfx

// ---- Arduino core stubs
struct SerialStub {
  void begin(int) {}
  void println(const char *s = "") { printf("[serial] %s\n", s); }
  void print(char) {}
  void print(const char *) {}
  void printf(const char *f, ...) {
    va_list a; va_start(a, f); vprintf(f, a); va_end(a);
  }
};
extern SerialStub Serial;
inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
inline void delay(int) {}
inline unsigned long millis() { return 0; }

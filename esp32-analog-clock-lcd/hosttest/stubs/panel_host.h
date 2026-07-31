// Host-side stand-in for the Arduino core plus the sketch's panel
// adapter. Compiled with -DHOSTTEST=1, the sketch declares the panel
// functions and this file supplies them, painting into a real 360x360
// framebuffer that the tests can inspect.
//
// Note what is NOT stubbed any more: lines, circles and glyphs are drawn
// by the sketch's own code now, so the tests exercise the real rendering
// rather than an approximation of a library's.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <cmath>

#define DEG_TO_RAD 0.017453292519943295f

static const int FB_W = 360, FB_H = 360;
extern uint16_t g_fb[FB_W * FB_H];
extern long g_pixelWrites;
extern int g_batchDepth;

inline void panelInit() {}

inline void panelDrawPixel(int16_t x, int16_t y, uint16_t color) {
  g_pixelWrites++;
  if (x < 0 || y < 0 || x >= FB_W || y >= FB_H) return;
  g_fb[y * FB_W + x] = color;
}

inline void panelFillRect(int16_t x, int16_t y, int16_t w, int16_t h,
                          uint16_t color) {
  for (int16_t j = 0; j < h; j++)
    for (int16_t i = 0; i < w; i++) panelDrawPixel(x + i, y + j, color);
}

inline void panelBeginBatch() { g_batchDepth++; }
inline void panelEndBatch() { g_batchDepth--; }

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

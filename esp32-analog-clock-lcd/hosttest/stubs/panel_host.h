// Host-side stand-in for the Arduino core plus the sketch's panel
// adapter. Compiled with -DHOSTTEST=1, the sketch declares the panel
// functions and this file supplies them, painting into a real 360x360
// framebuffer that the tests can inspect.
//
// Note what is NOT stubbed any more: lines, circles and glyphs are drawn
// by the sketch's own code now, so the tests exercise the real rendering
// rather than an approximation of a library's.
#pragma once
#include "arduino_core.h"

static const int FB_W = 360, FB_H = 360;
extern uint16_t g_fb[FB_W * FB_H];
extern long g_pixelWrites;
extern int g_batchDepth;
// Marks every pixel painted in the background colour during a frame.
// Cross-referenced afterwards against the second hand's final pixels: any
// overlap is a pixel that went dark and then came back, i.e. flicker. The
// trailing edge does not count - those pixels are meant to go dark.
extern uint16_t g_bgColor;
extern uint8_t g_bgTouched[];
// One "op" = one address window on the real panel, which is what actually
// costs time on this driver - not the pixel count.
extern long g_panelOps;
extern int g_inFillRect;

// Simulates a panel that occasionally loses a write: per-mille chance that
// an operation is silently discarded. A dropped rect loses the whole run,
// which is what a corrupted address-window command would do.
extern uint32_t g_dropPerMille;
extern uint32_t g_dropRng;
inline bool droppedWrite() {
  if (!g_dropPerMille) return false;
  g_dropRng = g_dropRng * 1664525u + 1013904223u;
  return ((g_dropRng >> 16) % 1000u) < g_dropPerMille;
}

inline void panelInit() {}

inline void panelDrawPixel(int16_t x, int16_t y, uint16_t color) {
  g_pixelWrites++;
  if (!g_inFillRect) g_panelOps++;
  if (color == g_bgColor && x >= 0 && y >= 0 && x < FB_W && y < FB_H)
    g_bgTouched[y * FB_W + x] = 1;
  if (x < 0 || y < 0 || x >= FB_W || y >= FB_H) return;
  if (!g_inFillRect && droppedWrite()) return;
  g_fb[y * FB_W + x] = color;
}

inline void panelFillRect(int16_t x, int16_t y, int16_t w, int16_t h,
                          uint16_t color) {
  g_panelOps++;
  if (droppedWrite()) return;
  g_inFillRect++;
  for (int16_t j = 0; j < h; j++)
    for (int16_t i = 0; i < w; i++) panelDrawPixel(x + i, y + j, color);
  g_inFillRect--;
}

inline void panelBeginBatch() { g_batchDepth++; }
inline void panelEndBatch() { g_batchDepth--; }

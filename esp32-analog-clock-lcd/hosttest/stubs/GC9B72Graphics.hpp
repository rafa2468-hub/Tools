// Stand-in for the real GC9B72Graphics.hpp, which is not in this repo.
//
// It mirrors the real file's *interface* - the pin defines and the symbols
// the panel adapter calls - so the firmware build path can be compiled and
// type-checked on the host. The register init sequence is deliberately
// absent; nothing here talks to hardware.
//
// Kept in sync by hand. If the real driver's pin names or function
// signatures change, this is the file that has to follow.
#pragma once
#include "arduino_core.h"

#define PIN_SCK 4
#define PIN_MOSI 6
#define PIN_CS 7
#define PIN_DC 2
#define PIN_RST 3

inline void sendCmd(uint8_t) {}
inline void sendData(uint8_t) {}
inline void setAddrWindow(uint16_t, uint16_t, uint16_t, uint16_t) {}
inline void drawPixel(int, int, uint16_t) {}
inline void drawLine(int, int, int, int, uint16_t) {}
inline void drawCircle(int, int, int, uint16_t) {}
inline void drawHand(int, int, float, int, uint16_t) {}
inline void gc9b72_init() {}

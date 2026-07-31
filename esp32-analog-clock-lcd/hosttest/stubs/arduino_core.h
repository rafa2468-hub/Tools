// The slice of the Arduino core the sketch touches. Shared by both host
// build configurations.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <cmath>

#define DEG_TO_RAD 0.017453292519943295f

#define HIGH 1
#define LOW 0
#define OUTPUT 1
#define INPUT 0
#define MSBFIRST 1
#define SPI_MODE0 0

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

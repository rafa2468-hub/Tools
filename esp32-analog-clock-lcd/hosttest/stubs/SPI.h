// Enough of the ESP32 Arduino SPI API for the panel adapter to compile.
// Nothing here transmits anything - the point is to type-check the real
// (non-HOSTTEST) firmware path on the host.
#pragma once
#include "arduino_core.h"

class SPISettings {
public:
  SPISettings() {}
  SPISettings(uint32_t, uint8_t, uint8_t) {}
};

class SPIClass {
public:
  void begin(int8_t sck = -1, int8_t miso = -1, int8_t mosi = -1,
             int8_t ss = -1) {}
  void beginTransaction(SPISettings) {}
  void endTransaction() {}
  uint8_t transfer(uint8_t data) { return data; }
  void writeBytes(const uint8_t *, uint32_t) {}
};

extern SPIClass SPI;

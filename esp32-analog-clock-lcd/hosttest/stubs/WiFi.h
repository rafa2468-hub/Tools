#pragma once
#include <cstdlib>
#include <ctime>

#define WL_CONNECTED 3
#define WIFI_STA 1

struct WiFiStub {
  int mode(int) { return 0; }
  void begin(const char *, const char *) {}
  int status() { return WL_CONNECTED; }
};
extern WiFiStub WiFi;

// Mirrors the ESP32 core: applies the TZ string verbatim, no DST rule
// synthesis. That is precisely the property main.cpp relies on.
inline void configTzTime(const char *tz, const char *, const char *) {
  setenv("TZ", tz, 1);
  tzset();
}
inline bool getLocalTime(struct tm *info, uint32_t = 5000) {
  time_t now = time(nullptr);
  localtime_r(&now, info);
  return true;
}

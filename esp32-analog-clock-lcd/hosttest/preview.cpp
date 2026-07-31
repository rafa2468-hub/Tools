// Renders the clock face to a PNG so it can be eyeballed without
// flashing hardware. Everything drawn here is the sketch's own code.
//
//   make preview        -> preview.png at 10:09:36
//   ./preview 3 25 40   -> a specific time
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

uint16_t g_fb[FB_W * FB_H];
long g_pixelWrites = 0;
int g_batchDepth = 0;
double g_testNow = 0;
SerialStub Serial;
WiFiStub WiFi;

#include "analog_clock.ino"

// ---- minimal PNG writer (zlib "stored" blocks, so no libz needed)
static uint32_t crcTable[256];
static void initCrc() {
  for (uint32_t n = 0; n < 256; n++) {
    uint32_t c = n;
    for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    crcTable[n] = c;
  }
}
static uint32_t crc32buf(const uint8_t *b, size_t n, uint32_t c = 0xFFFFFFFFu) {
  for (size_t i = 0; i < n; i++) c = crcTable[(c ^ b[i]) & 0xFF] ^ (c >> 8);
  return c;
}
static void be32(std::vector<uint8_t> &v, uint32_t x) {
  v.push_back(x >> 24); v.push_back(x >> 16); v.push_back(x >> 8); v.push_back(x);
}
static void chunk(FILE *f, const char *type, const std::vector<uint8_t> &data) {
  std::vector<uint8_t> hdr;
  be32(hdr, (uint32_t)data.size());
  fwrite(hdr.data(), 1, 4, f);
  std::vector<uint8_t> body(type, type + 4);
  body.insert(body.end(), data.begin(), data.end());
  fwrite(body.data(), 1, body.size(), f);
  std::vector<uint8_t> crc;
  be32(crc, crc32buf(body.data(), body.size()) ^ 0xFFFFFFFFu);
  fwrite(crc.data(), 1, 4, f);
}
static void writePng(const char *path) {
  initCrc();
  std::vector<uint8_t> raw;
  for (int y = 0; y < FB_H; y++) {
    raw.push_back(0); // filter: none
    for (int x = 0; x < FB_W; x++) {
      uint16_t c = g_fb[y * FB_W + x];
      raw.push_back(((c >> 11) & 0x1F) << 3);
      raw.push_back(((c >> 5) & 0x3F) << 2);
      raw.push_back((c & 0x1F) << 3);
    }
  }
  std::vector<uint8_t> z;
  z.push_back(0x78); z.push_back(0x01);
  size_t pos = 0;
  while (pos < raw.size()) {
    size_t n = std::min<size_t>(65535, raw.size() - pos);
    bool last = (pos + n == raw.size());
    z.push_back(last ? 1 : 0);
    z.push_back(n & 0xFF); z.push_back(n >> 8);
    z.push_back(~n & 0xFF); z.push_back((~n >> 8) & 0xFF);
    z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
    pos += n;
  }
  uint32_t a = 1, b = 0;
  for (uint8_t byte : raw) { a = (a + byte) % 65521; b = (b + a) % 65521; }
  be32(z, (b << 16) | a);

  FILE *f = fopen(path, "wb");
  const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  fwrite(sig, 1, 8, f);
  std::vector<uint8_t> ihdr;
  be32(ihdr, FB_W); be32(ihdr, FB_H);
  ihdr.push_back(8); ihdr.push_back(2);
  ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
  chunk(f, "IHDR", ihdr);
  chunk(f, "IDAT", z);
  chunk(f, "IEND", {});
  fclose(f);
}

int main(int argc, char **argv) {
  int hh = argc > 1 ? atoi(argv[1]) : 10;
  int mm = argc > 2 ? atoi(argv[2]) : 9;
  double ss = argc > 3 ? atof(argv[3]) : 36.5;

  setenv("TZ", TZ_INFO, 1);
  tzset();
  struct tm t = {};
  t.tm_year = 2026 - 1900; t.tm_mon = 6; t.tm_mday = 15;
  t.tm_hour = hh; t.tm_min = mm; t.tm_sec = 0; t.tm_isdst = -1;
  g_testNow = (double)mktime(&t) + ss;

  panelInit();
  struct tm now; float s;
  readClock(now, s);
  computeHands(now, s, hourHand, minHand, secHand);
  drawFace();
  drawHand(hourHand, COLOR_HOUR_HAND, HOUR_HAND_W);
  drawHand(minHand, COLOR_MIN_HAND, MIN_HAND_W);
  drawHub();
  drawHand(secHand, COLOR_SEC_HAND, SEC_HAND_W);

  writePng("preview.png");
  printf("wrote preview.png (%02d:%02d:%04.1f)\n", hh, mm, ss);
  return 0;
}

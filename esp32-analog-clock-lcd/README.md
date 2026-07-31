# esp32-analog-clock-lcd

An analog clock face for a 2.1" round 360x360 SPI TFT (GC9B72 driver IC),
driven by an ESP32-C3 Super Mini. Time comes from NTP over Wi-Fi; if
Wi-Fi isn't configured (or the connection fails), the sketch falls back
to the firmware's build timestamp so the clock still runs.

## Hardware

- ESP32-C3 Super Mini
- 2.1" round SPI TFT, 360x360, GC9B72 driver, 4-wire SPI

### Wiring

| Display pin | ESP32-C3 GPIO | Notes |
|---|---|---|
| SCK / SCL   | 4  | SPI clock |
| SDA / MOSI  | 6  | SPI data in |
| DC / RS     | 7  | data/command |
| CS          | 10 | chip select |
| RST         | 3  | reset |
| BLK / LED   | 5  | backlight enable, active HIGH |
| VCC         | 3V3 | **3.3V only** |
| GND         | GND | |

These pins just avoid the C3's boot-strapping pins (GPIO2/8/9) and its
native-USB pins (GPIO18/19); any other free GPIOs work equally well if you
rewire and update the `#define`s at the top of `src/main.cpp`.

## Driver note: GC9B72 vs GC9C01

At the time of writing, [Arduino_GFX](https://github.com/moononournation/Arduino_GFX)
has no driver named specifically for the GC9B72. It's a GalaxyCore round
panel controller from the same family as, and register-compatible with,
GC9C01 — which Arduino_GFX already supports natively at exactly 360x360.
This sketch uses `Arduino_GC9C01`, which is the correct choice for these
TZT/DIYUSER-style 2.1" round GC9B72 modules in practice.

If your panel shows something wrong out of the box:

- **Mirrored or rotated image**: change the rotation argument in the
  `Arduino_GC9C01(...)` constructor (0-3), or try `setRotation()` in
  `setup()` after `gfx->begin()`.
- **Image offset / partially off-screen**: pass non-zero
  `col_offset1`/`row_offset1` to the `Arduino_GC9C01` constructor (see the
  driver's header for the full parameter list).
- **Inverted colors**: toggle the `ips` constructor argument (currently
  `true`).

## Building and flashing

This is a [PlatformIO](https://platformio.org/) project.

```sh
pio run                # build
pio run -t upload      # build and flash
pio device monitor      # serial monitor (115200 baud)
```

The ESP32-C3 Super Mini exposes only the native USB-Serial/JTAG
peripheral (no separate USB-UART bridge chip), which is why
`platformio.ini` sets `ARDUINO_USB_CDC_ON_BOOT=1` — without it, `Serial`
output over USB won't appear.

## Configuration

Edit the constants at the top of `src/main.cpp` before flashing:

- `WIFI_SSID` / `WIFI_PASSWORD` — leave `WIFI_SSID` as
  `"YOUR_WIFI_SSID"` to skip Wi-Fi/NTP entirely and run from the
  compile-time fallback clock instead.
- `GMT_OFFSET_SEC` / `DAYLIGHT_OFFSET_SEC` — your local time zone, as
  offsets from UTC in seconds (see the comment above them for examples).
- `NTP_SERVER_1` / `NTP_SERVER_2` — NTP sources.

## How it works

- `drawFace()` renders the dial (bezel, 60 tick marks, 12 numerals) once
  at startup.
- Each second, `loop()` reads the current time, computes new hand-tip
  coordinates, erases the previous hour/minute/second hands by redrawing
  them in the background color, then draws the hands at their new
  positions. Hand and tick geometry are sized so the hands never reach
  the tick/numeral ring, so the static face never needs to be touched
  after startup — only the three hands and the center hub redraw each
  second.
- Time itself is supplied by the ESP32 core's SNTP client
  (`configTime`), which keeps the system clock synced in the background
  after the initial connection; `loop()` just reads it with
  `getLocalTime()`.

## Limitations

- No RTC battery backup: on power loss the ESP32-C3 has no way to keep
  time, so it re-syncs over Wi-Fi (or falls back to build time) on every
  boot.
- The fallback (no-Wi-Fi) clock is only as accurate as the moment the
  firmware was built, and drifts with the ESP32's crystal tolerance from
  there.

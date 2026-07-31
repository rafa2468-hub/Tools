# esp32-analog-clock-lcd

An analog clock face for a 2.1" round 360x360 SPI TFT (GC9B72 driver IC),
driven by an ESP32-C3 Super Mini. Time comes over Wi-Fi from an NTP
server on the LAN (`192.168.1.5`); if Wi-Fi isn't configured, or the
connection or sync fails, the sketch falls back to the firmware's build
timestamp so the clock still runs.

## Layout

```
analog_clock/analog_clock.ino   the sketch (Arduino IDE opens this)
analog_clock/LGFX_GC9B72.hpp    vendor panel definition - SUPPLY THIS YOURSELF
platformio.ini                  PlatformIO build config, points at the above
hosttest/                       host-side tests, no hardware needed
```

`LGFX_GC9B72.hpp` ships with the display module and is not in this repo —
see [Driver](#driver-lovyangfx--the-vendor-gc9b72-panel-definition) below.

## Hardware

- ESP32-C3 Super Mini
- 2.1" round SPI TFT, 360x360, GC9B72 driver, 4-wire SPI

### Wiring

| Display pin | ESP32-C3 GPIO | Notes |
|---|---|---|
| SCK / SCL   | 4   | SPI clock |
| SDA / MOSI  | 6   | SPI data in |
| CS          | 7   | chip select |
| DC / RS     | 2   | data/command (strapping pin, see below) |
| RST         | 3   | reset |
| MISO        | —   | not connected; the display is write-only |
| BLK / LED   | —   | on by default; LovyanGFX drives it if the panel file configures it |
| VCC         | 3V3 | **3.3V only** |
| GND         | GND | |

This is the wiring the module's own demo was verified on. Under LovyanGFX
the pins are set in `LGFX_GC9B72.hpp`, not in the sketch — to rewire, edit
that file.

The BLK/LED pin is left unwired and the backlight comes up on regardless,
so the module pulls it high itself. For software brightness control,
connect it to a free GPIO and add a `Light_PWM` block to
`LGFX_GC9B72.hpp`.

### GPIO2 and boot

GPIO2 is one of the ESP32-C3's strapping pins: it is sampled at reset to
choose the boot mode and must not be held LOW at that instant. A TFT's DC
line is a high-impedance input, so in practice it doesn't disturb the
strap — and this board does boot reliably with the display attached. Noted
only because if it ever *doesn't*, this is the first thing to suspect:
move DC to a non-strapping GPIO (1, 5 and 10 are free in this wiring) and
update the pin in `LGFX_GC9B72.hpp`.

## Driver: LovyanGFX + the vendor GC9B72 panel definition

The sketch needs two vendor files alongside it in `analog_clock/`:

```
analog_clock/LGFX_GC9B72.hpp    panel definition: pins, SPI config, init sequence
analog_clock/GC9B72Graphics.hpp (if the vendor demo includes one)
```

These ship with the display module and are **not** part of LovyanGFX
upstream — there is no public GC9B72 driver. `LGFX_GC9B72.hpp` is what
carries the controller's register init sequence, and it also owns the pin
assignments, so the wiring is configured there rather than in the sketch.

### Why not Arduino_GFX

An earlier version of this sketch drove the panel with Arduino_GFX's
`Arduino_GC9C01`, on the reasoning that GalaxyCore's two 360x360 round
controllers were register-compatible. **They are not.** That build left
the panel powered and backlit but showing uninitialized display RAM — a
uniform fine dither, no image. The bus was never the problem: these
modules are 4-wire SPI, the wiring was correct, and the vendor's own demo
ran on the identical wiring. Only the init sequence was wrong, and it is
not something you can guess at.

If your panel shows something wrong once it *is* initializing:

- **Mirrored or rotated image**: change `gfx.setRotation(0)` in `setup()`
  (0-3).
- **Image offset / partially off-screen**: adjust `panel_cfg.offset_x` /
  `offset_y` in `LGFX_GC9B72.hpp`.
- **Inverted colors**: toggle `panel_cfg.invert` in `LGFX_GC9B72.hpp`.

## Building and flashing

The sketch is a single file, `analog_clock/analog_clock.ino`. It builds
either from the Arduino IDE or from PlatformIO — there is only one copy of
the source, so the two can't drift apart.

### Arduino IDE

1. **Board support**: in *File → Preferences → Additional boards manager
   URLs* add
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`,
   then install **esp32** by Espressif Systems from *Tools → Board →
   Boards Manager*.
2. **Library**: in *Tools → Manage Libraries*, install **LovyanGFX** by
   lovyan03. Also make sure the vendor's `LGFX_GC9B72.hpp` (and
   `GC9B72Graphics.hpp` if the demo used one) sit in the `analog_clock`
   folder next to the sketch — they carry the GC9B72 init sequence and the
   pin assignments.
3. **Open** `analog_clock/analog_clock.ino` (keep it in its
   `analog_clock` folder — the IDE requires the folder and the sketch to
   share a name).
4. **Board settings** under *Tools*:
   - Board: **ESP32C3 Dev Module**
   - **USB CDC On Boot: Enabled** ← see the note below
   - Flash Size: 4MB, Partition Scheme: default
5. Select the port and hit Upload. Serial Monitor at 115200 baud.

If the board isn't detected, hold **BOOT**, tap **RESET**, release
**BOOT** to force the bootloader, upload, then tap **RESET** again.

### PlatformIO

```sh
pio run                # build
pio run -t upload      # build and flash
pio device monitor     # serial monitor (115200 baud)
```

`platformio.ini` points `src_dir` at `analog_clock/` and pulls LovyanGFX
in automatically. The vendor `LGFX_GC9B72.hpp` still has to be present in
`analog_clock/`.

### The USB CDC setting

The ESP32-C3 Super Mini exposes only the native USB-Serial/JTAG
peripheral — there is no separate USB-UART bridge chip. `Serial` therefore
has to be routed over native USB, which is what **USB CDC On Boot:
Enabled** does in the Arduino IDE, and what `ARDUINO_USB_CDC_ON_BOOT=1` in
`platformio.ini` does for PlatformIO. Without it the sketch still runs,
but the serial output (Wi-Fi and NTP progress) never appears.

## Configuration

Edit the constants at the top of `analog_clock/analog_clock.ino` before flashing:

- `WIFI_SSID` / `WIFI_PASSWORD` — leave `WIFI_SSID` as
  `"YOUR_WIFI_SSID"` to skip Wi-Fi/NTP entirely and run from the
  compile-time fallback clock instead.
- `SMOOTH_SECONDS` — `true` (default) sweeps the second hand
  continuously; `false` steps it once per second like a quartz movement.
- `TZ_INFO` — local time zone as a POSIX TZ string. Defaults to
  `"CET-1CEST,M3.5.0,M10.5.0/3"` (Europe/Warsaw). See the note below
  before changing it.
- `NTP_SERVER_1` / `NTP_SERVER_2` — time sources. `NTP_SERVER_1` defaults
  to `192.168.1.5`, a local NTP server on the LAN, so the clock syncs
  without needing internet access. `NTP_SERVER_2` is `pool.ntp.org`,
  consulted only if the local server doesn't answer; set it to `nullptr`
  if this device should never reach outside the LAN.

The Wi-Fi network you point it at must of course be able to route to
`192.168.1.5`.

### Time zones and DST

The sketch calls `configTzTime(TZ_INFO, ...)` with an explicit POSIX TZ
string rather than the more commonly seen
`configTime(gmtOffset, daylightOffset, ...)`. This is deliberate.

`configTime()`'s offset form builds a TZ string containing only offsets
and abbreviations — no DST *transition rule*. The C library then falls
back to its built-in default, which is the **US** switching schedule. The
EU switches on the last Sunday of March and the last Sunday of October,
while the US switches on the second Sunday of March and the first Sunday
of November, so a clock configured that way reads an hour wrong for
roughly two weeks each spring and one week each autumn.

`"CET-1CEST,M3.5.0,M10.5.0/3"` spells the EU rule out: CET at UTC+1
(POSIX inverts the sign), CEST one hour ahead, starting on month 3, week
5 ("last"), day 0 (Sunday), and ending month 10, last Sunday, at 03:00
local. Transitions are then handled correctly with no code change.

For another zone, use that zone's TZ string — e.g. `GMT0BST,M3.5.0/1,M10.5.0`
for the UK, `EST5EDT,M3.2.0,M11.1.0` for US Eastern.

## How it works

- `drawFace()` renders the dial (bezel, 60 tick marks, 12 numerals) once
  at startup. It is never repainted wholesale after that.
- `loop()` reads the clock with microsecond resolution and places the
  second hand at a fractional angle, so it sweeps continuously rather
  than stepping. Redraws are triggered by *pixels*, not by a timer: a new
  frame is drawn only when a rounded hand coordinate actually changes. At
  the second hand's length that works out to roughly one frame every
  66 ms — the smoothest motion the panel can resolve, with no wasted
  redraws in between.
- Erasing a hand means painting it in the background color, which damages
  anything it was lying on top of. So each frame repairs what it
  disturbed: the numeral the second hand was crossing (at most one, found
  by angle), plus the other two hands and the center hub, which are
  simply repainted unconditionally — drawing is idempotent, and at under
  a thousand pixels it is cheaper than reasoning about whether they
  overlapped.
- The whole sweep costs about 28k pixel writes per second, roughly 2% of
  what the SPI link can carry, so there is plenty of headroom.
- Time itself is supplied by the ESP32 core's SNTP client
  (`configTzTime`), which keeps the system clock synced in the background
  after the initial connection; `loop()` just reads it with
  `getLocalTime()`.

## Tests

The rendering logic is pure computation, so it can be exercised on a
development machine with no board attached:

```sh
cd hosttest && make
```

This compiles `analog_clock/analog_clock.ino` against stubbed
Arduino/LovyanGFX headers
that paint into a real 360x360 framebuffer, and drives it from a
test-controlled clock so a full 60-second sweep runs instantly.

The central check is that the incremental renderer — which only touches
what moved — leaves the framebuffer **byte-identical to a full repaint of
the same instant**, and that this holds on *every* frame, not just at the
end. Comparing only the final frame is not enough: corruption that
appears and is healed a moment later still shows up as a flicker on the
dial, and an earlier version of this code had exactly that bug (the
second hand clipped the minute hand near the center, where a 3px-wide
hand subtends ~19 degrees).

If you change the face geometry — hand lengths, numeral radius, text size
— run this afterwards. Lengthening a hand until it reaches a face element
it did not previously touch is the easiest way to reintroduce the problem.

`make` also runs an **ino-check**: the Arduino IDE rewrites a `.ino`
before compiling, generating prototypes for every function and injecting
them ahead of the first function definition. If a type used in a
signature is declared below that point, the IDE build fails with a wall of
`'X' has not been declared` — while PlatformIO, which compiles the file as
ordinary C++, stays perfectly green. `ino_preprocess.py` reproduces that
rewrite so the mismatch is caught here rather than in the IDE. This is why
`struct Hand` sits above every function in the sketch.

## Limitations

- No RTC battery backup: on power loss the ESP32-C3 has no way to keep
  time, so it re-syncs over Wi-Fi (or falls back to build time) on every
  boot.
- The fallback (no-Wi-Fi) clock is only as accurate as the moment the
  firmware was built, and drifts with the ESP32's crystal tolerance from
  there.

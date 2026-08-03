# esp32-analog-clock-lcd

An analog clock face for a 2.1" round 360x360 SPI TFT (GC9B72 driver IC),
driven by an ESP32-C3 Super Mini. Time comes over Wi-Fi from an NTP
server on the LAN (`192.168.1.5`); if Wi-Fi isn't configured, or the
connection or sync fails, the sketch falls back to the firmware's build
timestamp so the clock still runs.

## Layout

```
analog_clock/analog_clock.ino   the sketch (Arduino IDE opens this)
analog_clock/GC9B72Graphics.hpp GC9B72 driver - SUPPLY THIS YOURSELF
platformio.ini                  PlatformIO build config, points at the above
hosttest/                       host-side tests, no hardware needed
```

`GC9B72Graphics.hpp` is not in this repo — see [Driver](#driver) below.

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
| BLK / LED   | —   | not wired; backlight is on by default |
| VCC         | 3V3 | **3.3V only** |
| GND         | GND | |

This is the wiring the module's own demo was verified working on. The
pins are configured in your display driver, not in this sketch.

The BLK/LED pin is left unwired and the backlight comes up on regardless,
so the module pulls it high itself.

### GPIO2 and boot

GPIO2 is one of the ESP32-C3's strapping pins: it is sampled at reset to
choose the boot mode and must not be held LOW at that instant. A TFT's DC
line is a high-impedance input, so in practice it doesn't disturb the
strap — and this board does boot reliably with the display attached. Noted
only because if it ever *doesn't*, this is the first thing to suspect:
move DC to a non-strapping GPIO (1, 5 and 10 are free in this wiring).

## Driver

**This sketch depends on no graphics library.** Everything on the face —
lines, circles, the numerals — is drawn from two primitives: fill a
rectangle, and set one pixel. That is a deliberate response to two failed
attempts to reuse an existing driver for this panel (see below).

It expects `GC9B72Graphics.hpp` in the `analog_clock` folder — a small
hand-written driver holding the GC9B72 register init sequence, the pin
definitions, and `setAddrWindow`. That file is not in this repo.

Everything display-specific is in the **PANEL ADAPTER** block at the top
of the sketch:

| Function | What it does |
|---|---|
| `panelInit()` | pins, reset pulse, `SPI.begin()`, then `gc9b72_init()` |
| `panelDrawPixel(x, y, color)` | one RGB565 pixel |
| `panelFillRect(x, y, w, h, color)` | one address window, then streams |
| `panelBeginBatch()` / `panelEndBatch()` | one SPI transaction per frame |

Two things worth knowing if you adapt this to another driver:

- **`panelInit()` does the bus setup.** `GC9B72Graphics.hpp` only sends
  the register sequence — it never calls `pinMode`, `SPI.begin()`, or
  pulses reset. Those have to happen before `gc9b72_init()` or nothing
  reaches the panel.
- **`panelFillRect` is not a convenience.** The clock draws in horizontal
  runs, and the driver's own `drawPixel` opens a fresh address window per
  pixel — around 24 `digitalWrite` calls each. Going through one window
  per *run* instead of per pixel is what makes a 15fps sweep feasible at
  all; the same applies to the 129,600-pixel background fill.

Colors are RGB565, the same format nearly every SPI TFT wants.

### Two drivers that did not work

Recorded because it cost real bench time:

- **Arduino_GFX's `Arduino_GC9C01`.** GC9C01 is GalaxyCore's other
  360x360 round controller and Arduino_GFX supports it natively, so it
  looked like a safe substitute. It is not: the two are **not**
  register-compatible. The panel stayed backlit showing uninitialized
  display RAM — a uniform fine dither, no image.
- **LovyanGFX**, including with the vendor's own `LGFX_GC9B72.hpp`.

The bus was never the problem. These modules are 4-wire SPI, the wiring
below is correct, and the vendor's own demo ran on that identical wiring.
Only the init sequence was ever wrong — and there is no public GC9B72
driver to substitute in, so the working init sequence has to come from
your own driver.

### Stray pixels / comet trails

Scattered pixels left behind along the second hand's path are a **bus
integrity** symptom, not a drawing bug. The erase pass tells the panel to
clear those pixels; if the address-window command that precedes the write
is corrupted, the write lands elsewhere and the original pixel stays lit.

`GC9B72Graphics.hpp` never calls `beginTransaction`, so the vendor demo
ran at the Arduino default of **1MHz**. This sketch sets `PANEL_SPI_HZ`
explicitly; it was originally 40MHz, which proved too aggressive on
dupont jumper wires — strays appeared in bursts, consistent with Wi-Fi
transmit activity disturbing the supply. It is now 10MHz.

If strays still appear, **keep halving `PANEL_SPI_HZ`** (4MHz, 2MHz). The
sweep is nowhere near bus-limited — a frame's cost is dominated by
per-window GPIO overhead, and at 10MHz the bus is under 5% loaded — so
there is nothing to lose. Shorter wiring and a solid 3V3/GND pair are what
actually buy headroom here, not a bigger number.

Note that existing strays do not always self-clear: the hand sweeps the
same ray a minute later and cleans up pixels that lie exactly on its path,
but one rounded a pixel off it will persist. A reset repaints the face.

### If the image appears but looks wrong

- **Mirrored or rotated**: adjust the MADCTL value (`cmd(0x36)`) in the
  init sequence, or swap/invert the axes in `panelOpenWindow`.
- **Offset**: add the panel's offset in `panelOpenWindow`.
- **Colors inverted or swapped (red/blue)**: your driver's pixel format
  differs — either flip the invert bit in its init, or byte-swap in
  `panelDrawPixel`.

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
2. **Display driver**: no library to install — put `GC9B72Graphics.hpp`
   in the `analog_clock` folder next to the sketch (see
   [Driver](#driver)).
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

`platformio.ini` points `src_dir` at `analog_clock/`. There are no library
dependencies; `GC9B72Graphics.hpp` just has to be in that folder.

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
- **The second hand is never erased.** Consecutive sweep positions are
  about 0.4° apart, so the two lines pick the same pixel everywhere inside
  r≈70 and differ only towards the tip. Each frame collects the new
  position's pixels, and only the old ones the new position does not reuse
  get painted back to background. See "Why the hand is not erased" below.
- Erasing anything damages whatever it was lying on. Each erased pixel is
  tested against the hour hand, the minute hand and the numeral ring, and
  only what was actually hit gets repaired. When anything is repaired the
  whole stack is replayed in order — numerals, hour, minute, hub — because
  restoring just the damaged item would put it on top of things that
  should cover it.
- The sweep costs about 27k pixel writes per second, but the number that
  matters on this driver is ~4.5k address windows per second: each one is
  ~6 `digitalWrite`s and 11 SPI byte transfers, so roughly 20µs. That is
  under 10% of the time budget.
- Time itself is supplied by the ESP32 core's SNTP client
  (`configTzTime`), which keeps the system clock synced in the background
  after the initial connection; `loop()` just reads it with
  `getLocalTime()`.

### Why the hand is not erased

The panel has no back buffer. A pixel painted to background and then
repainted in the same frame is genuinely dark on the glass for however
long that frame takes — and a frame here is milliseconds, with background
Wi-Fi work occasionally stretching it.

The first version erased the whole second hand, then repaired the numerals
and both other hands, and only repainted the second hand at the very end.
All ~165 of its pixels were therefore dark for the entire frame. Filmed at
30fps, the hand was missing or half-drawn in 4 of 149 frames — twice
absent outright. That is the stagger.

Not erasing it fixes the cause rather than the odds: 2.6 pixels blink per
frame now instead of 165, and `make` asserts that number stays low. The
framebuffer comparison cannot catch this on its own — both versions end
every frame with a correct image, and it only looks at end states.

## Tests

The rendering logic is pure computation, so it can be exercised on a
development machine with no board attached:

```sh
cd hosttest && make
```

This compiles `analog_clock/analog_clock.ino` with only `panelDrawPixel`
stubbed — painting into a real 360x360 framebuffer — and drives it from a
test-controlled clock, so a full 60-second sweep runs instantly. Because
the sketch owns its drawing code, the tests exercise the real line, circle
and glyph routines rather than a library's.

`make preview` renders the face to `preview.png` using that same code, so
you can see what the panel will show before flashing. Pass a time with
`make preview T="3 25 40"`. This is worth doing after any geometry change:
it is how the hollow-hand bug below was caught.

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

Note what the framebuffer comparison alone will *not* catch: it checks
that the incremental render matches a full render, so a drawing routine
that is wrong in the same way in both passes still compares equal. The
thick-line renderer had exactly that bug — hands drawn as stacked
Bresenham lines came apart into loose strands on diagonals, and every test
passed because the erase pass came apart identically. `make preview` is
what surfaced it. Hands are now drawn as filled quads.

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

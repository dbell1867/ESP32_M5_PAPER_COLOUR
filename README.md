# M5Stack PaperColor — learning ESP32 + colour e-paper in C++

A hands-on learning project: bringing up an **M5Stack PaperColor** (SKU C151, ESP32-S3
with a 4" six-colour E Ink Spectra 6 display) from a factory-fresh unit to a
battery-powered temperature/humidity dashboard — written in **C++ with PlatformIO**,
by someone coming from Python/CircuitPython.

Everything is documented as a series of **lessons** (`docs/`), each with the real bugs,
measurements and dead ends — not just the final code.

<!-- photo: add one of the dashboard here, e.g. docs/img/dashboard.jpg -->

## What the firmware does now

An e-paper **room dashboard** that runs for weeks on the internal battery:

- shows the date, temperature (ink colour by range: blue < 18 °C, black, red > 25 °C),
  humidity, min/max, and "Updated HH:MM BST"
- refreshes the e-paper only when needed: every 10 min, or early if the temperature
  moves ≥ 0.5 °C
- between updates the board is **fully off** — the power chip (M5PM1) cuts everything
  except itself and the RTC, and its timer switches the board back on every 2 minutes
- press the **power button** (or plug in USB) for an immediate refresh and a 30 s
  window to download the battery log over serial
- **keeps its own clock right:** once a day it joins Wi-Fi for a few seconds, fetches
  network time (NTP), records how far the RTC had drifted, and corrects it
- **photo frame:** `SHOW <name>` puts a photo from the SD card on the panel, dithered on the
  computer with Floyd–Steinberg into the six real inks (`tools/photo_prep.py`)
- **battery protection:** red "low battery" banner below 3.50 V, a clean "Battery empty"
  shutdown below 3.30 V, and the power chip's own cut-off raised from 2.5 V to 3.1 V

Measured on battery (rough, from voltage slope): ~**3 mA** average in standby mode vs
~38 mA with ordinary ESP32 deep sleep — roughly 2–3 weeks per charge (see Lesson 04).

## Hardware

| | |
|---|---|
| Board | M5Stack PaperColor (C151) |
| MCU | ESP32-S3R8 — 16 MB flash, 8 MB octal PSRAM |
| Display | 4" E Ink Spectra 6, 400×600, 6 inks, 14–39 s full refresh (see below) |
| Also used | SHT40 temp/humidity, RX8130CE RTC, M5PM1 power chip, 3 buttons, 2 RGB LEDs, microSD |
| Battery | 1250 mAh LiPo |

## Build and flash

Requires [PlatformIO](https://platformio.org/) (Core 6.1.x is fine — the platform is
pinned to pioarduino `55.03.311` for that reason).

```bash
pio run                  # build
pio run -t upload        # flash (board must be awake — see below)
```

The board spends most of its time **switched off**, so its USB port disappears. To
flash: start the upload, then press the **power button** — it wakes with a 30 s window.
If that fails, hold the side button ~3 s for download mode.

Read logs from a sleeping board:

```bash
~/.platformio/penv/bin/python tools/pull_log.py docs/data/battery-<date>.csv
# then press the power button
```

Libraries (pinned in `platformio.ini`): M5Unified 0.2.24, M5GFX 0.2.31.

## Repository layout

| Path | What |
|---|---|
| `src/` | current firmware (`main.cpp`) + `Sht40` sensor driver class |
| `include/board_pins.h` | pin map as `constexpr` |
| `stages/` | snapshot of `main.cpp` at each stage/lesson (copy one over `src/main.cpp` to try it) |
| `docs/lesson-*.md` | the lessons (start here) |
| `docs/PLAN.md` | roadmap: current position, decisions, open questions |
| `docs/data/` | measured data (fridge test, battery drain logs) |
| `docs/ref/` | M5Stack's schematic and M5PM1 datasheet (© M5Stack) |
| `tools/` | host-side Python helpers (see below) |
| `backup/` | verified dump of this unit's factory flash + SHA-256 |

## The lessons

1. **[Bring-up](docs/lesson-01-papercolor-bringup.md)** — backing up the factory
   firmware (and why esptool failed at the same addresses), first light on e-paper,
   probing buttons, microSD on a shared SPI bus, LED-for-feedback / e-paper-for-results.
2. **[Inside the driver](docs/lesson-02-inside-the-driver.md)** — reading M5GFX's
   `Panel_ED2208`: init bytes, timing a refresh with an interrupt, the 720 KB frame
   buffer, why mid-grey becomes green, dithering — predicted with a Python simulator,
   then verified on the glass.
3. **[Sensors and RTC](docs/lesson-03-sensors-and-rtc.md)** — I²C bus ownership, an
   SHT40 driver written from the datasheet, UTC/BST and clock sync, the dashboard, and
   a fridge experiment on refresh speed vs temperature.
4. **[Deep sleep and real power](docs/lesson-04-deep-sleep.md)** — deep sleep, a power
   bench, why a USB meter can't see this board's load, a 24 h battery test that found
   ~35 mA hiding in "deep sleep", the vendor schematic, and the power chip's standby
   mode (~10× less drain), then battery protection.
5. **[Wi-Fi and NTP](docs/lesson-05-wifi-ntp.md)** — scanning first, keeping the Wi-Fi
   password out of code and git, SNTP, and measuring the RTC's drift (~1.1 s/day).
6. **[A photo on six inks](docs/lesson-06-photo.md)** — error diffusion vs the driver's
   dithering (previewed, then confirmed on the glass), an investigation into the panel's
   refresh time (14.4 s → 14–39 s since 7 Oct), and light sleep while the panel refreshes.

## Tools

| Script | Purpose |
|---|---|
| `tools/epd_sim.py` | simulates the driver's colour/dither modes → PNGs, to predict what the panel will show |
| `tools/rtc_sync.py` | sets the RTC to the host's UTC on a second boundary and measures the offset |
| `tools/follow_serial.py` | follows serial output across sleeps (reopens the port) |
| `tools/pull_log.py` | waits for a service window and downloads the dashboard's logs |
| `tools/service.py` | sends commands during a service window (`LOG`, `SCAN`, `SYNC`, `SYNCLOG`, `WIFI?`, `SIMV 3.40` to simulate a battery voltage, `SIMV 0`) |
| `tools/photo_prep.py` | crop/resize a photo, preview the driver's dithering vs Floyd–Steinberg, write the panel image |
| `tools/patch_m5gfx.py` | pre-build script: M5GFX e-paper busy timeout 20 s → 60 s, and a light-sleep hook in its busy wait |
| `tools/wifi_setup.py` | stores Wi-Fi credentials on the board — asks for the password without showing it; run in your own terminal |

## Restoring the factory firmware

`backup/papercolor-factory-16MB.bin` is a verified image of **this particular unit's**
original flash:

```bash
esptool --chip esp32s3 --port /dev/ttyACM0 write-flash 0 backup/papercolor-factory-16MB.bin
```

## Findings worth knowing if you own this board

- The board **always runs from its battery**; USB only charges it — an inline USB power
  meter cannot see the board's consumption once the battery is full.
- M5Unified leaves the **Grove 5 V output on** by default (~24 mA with nothing attached).
- Ordinary ESP32 deep sleep keeps the main 3.3 V converter running; the vendor's 92 µA
  "standby" needs the **M5PM1 power chip to power everything off** and wake the board
  with its own timer.
- M5GFX owns Arduino's global `SPI` (use it for the SD card), and M5Unified's internal
  I²C bus is port 1 — `Wire.begin(3, 2)` would steal it.
- G1 is a button, **not** an SD card-detect pin (the docs say both); card-detect is the power chip's GPIO1.
- Full e-paper refresh was ≈ 14.4 s of panel time (16.1 s per `display()`) at every
  temperature tested; since 7 Oct this unit picks one of several programs at each power-up
  — **14.2 / 25 / 26.7 / 33–39 s** (cause unknown; forcing a temperature has no effect).
  M5GFX's busy wait gives up at 20 s and then powers the panel off mid-refresh —
  `tools/patch_m5gfx.py` raises it to 60 s.
- An inserted microSD card slows the e-paper's data transfer on the shared SPI bus.

## Status

Active learning project. Lessons 01–06 complete; the standby dashboard ran a 24 h
battery test with no missed wakes (see `docs/PLAN.md` for what's next).

## License

The code, tools and lesson docs in this repository are released under the
[MIT License](LICENSE).

Not covered by it — these belong to their owners and are included for reference only:

- `docs/ref/` — M5Stack's PaperColor schematic and M5PM1 datasheet (© M5Stack)
- `backup/` — a dump of M5Stack's factory firmware for this unit (© M5Stack)
- third-party libraries fetched by PlatformIO (M5Unified, M5GFX, the Arduino core),
  which keep their own licenses

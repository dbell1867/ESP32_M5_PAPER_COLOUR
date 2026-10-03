# Lesson 01 — Bringing Up a Colour E-Paper Board

**Project:** M5Stack PaperColor (SKU C151) in C++ with PlatformIO
**Goal of this lesson:** Go from a factory-fresh e-paper device to your own C++
firmware that draws in all six inks, reads three buttons, uses the RGB LEDs and
mounts a microSD card — while keeping a guaranteed way back to the factory
firmware.

> Audience note: written for someone comfortable with **Python / CircuitPython**
> who already brought up an LCD board (the LilyGO T-Display S3 Pro) and is now
> meeting **e-paper** for the first time. C++ is translated from Python where
> it helps. "Gotcha N" refers to the numbered playbook in the
> `esp32-board-bringup` skill.

---

## Learning objectives

By the end of this lesson you can:

1. Back up (and restore) an ESP32's entire flash before replacing its firmware —
   including when the read keeps failing.
2. Explain why we chose **M5Unified/M5GFX** over writing a low-level driver, and
   what "hybrid" means here.
3. Explain how e-paper differs from an LCD, and the **auto-display trap** that
   follows from it.
4. Draw exactly the panel's six inks, and explain why an LED needs *different*
   RGB values for the "same" colour.
5. Probe buttons properly: idle level, external vs internal pull-ups, active
   level, rockers.
6. Share one SPI bus between the display and an SD card **the way this library
   expects**, and diagnose what happens when you don't.
7. Treat vendor documentation as a hypothesis — and test it.

---

## The hardware

**M5Stack PaperColor (C151)**

| Part | Detail |
|---|---|
| MCU | ESP32-S3R8 — dual-core 240 MHz, **16 MB flash**, **8 MB octal PSRAM** |
| Display | 4" **E Ink Spectra 6** (ED2208 / EL040EF1), **400 × 600** portrait, 6 inks: black, white, yellow, red, blue, green. **No backlight, no touch.** |
| Input | 3 push buttons |
| Output | 2 RGB LEDs, speaker (AW8737A amp + ES8311 codec), IR transmitter |
| Sensors | SHT40 temperature/humidity, RX8130CE real-time clock, mic (ES7210) |
| Storage | microSD (on the display's SPI bus) |
| Power | M5PM1 power-management chip (PMIC), 1250 mAh battery, USB-C |
| USB | Native ESP32-S3 USB (`303a:1001`) → `/dev/ttyACM0` |

### Pin map (docs, then **verified on the hardware**)

| Function | Pin | Notes |
|---|---|---|
| E-paper SPI CLK / MOSI | G15 / G13 | shared with SD |
| E-paper CS / DC / RST / BUSY | G44 / G43 / G12 / G11 | |
| E-paper **power** | PMIC pin PYG0 | **not a GPIO** — M5GFX turns it on over I²C |
| SD CS / MISO | G47 / G14 | SD **power** also comes from the PMIC |
| Button A | G10 | left-top, nearer the corner |
| Button B | G9 | left-top, second |
| Button C | G1 | top-centre. Docs also call this "SD card-detect" — **false** (see Module 6) |
| RGB LEDs | G21 | 2 LEDs, addressable (driven over RMT) |
| Audio power / speaker enable | G45 / G46 | both **active HIGH** — we hold them LOW |
| I²C (SDA / SCL) | G3 / G2 | ES8311 0x18, ES7210 0x40, SHT40 0x44, RTC 0x32, PMIC 0x6E |

Physical orientation with M5GFX's default rotation: USB-C bottom-left, the
two buttons at the left of the top edge, and (0,0) — the top-left pixel — next
to them.

---

## Design decisions (made before any code)

### Don't touch the firmware until there's a way back

The board arrived running M5Stack's factory demo. Before flashing anything we
made a **complete, verified backup** (Module 1). Every later experiment is
reversible with one command.

### Library: M5Unified/M5GFX vs. a low-level driver

We read the library source before deciding. M5GFX **already supports this exact
board**: it auto-detects it and ships a dedicated panel driver,
`Panel_ED2208` (~560 lines).

| Problem | M5Unified/M5GFX | Write it yourself |
|---|---|---|
| Panel init | Done (≈15 vendor commands, e.g. `0xAA 0x49 0x55…`) | You copy the *same* undocumented bytes anyway |
| Panel power | M5Unified switches the PMIC | Raw I²C register writes; a bug = blank screen |
| Colour | RGB → 6 inks, with optional dithering | Hundreds of lines of colour maths |
| Drawing | Text, fonts, shapes, JPG/PNG, rotation | Raw pixels only |
| Board extras | Buttons, LEDs, speaker, RTC, power | Each one its own project |
| Panel safety | Tested power-on/off sequence | E-paper can be stressed by wrong waveforms or leaving it powered |

**Decision — hybrid:** *build* with M5Unified/M5GFX; *learn* the low level by
**reading `Panel_ED2208`** together in a later lesson; optionally write a
minimal driver afterwards as an exercise, with the library as a known-good
reference. Low-level-first would not really be "first principles" — the most
important part (the init bytes) comes from the vendor either way.

### Silence

The factory demo beeped every time the board reset — and esptool resets it on
every connection. Our firmware's **first two lines** drive the codec power
(G45) and speaker amplifier (G46) LOW. We checked M5Unified's PaperColor code to
confirm both are active-HIGH (so LOW really means off), and we tell M5Unified
not to set up the speaker or mic at all.

---

## Module 1 — Back up the factory firmware (and why it fought back)

`esptool read-flash` only **reads** — the safest possible first contact.

```bash
esptool --chip esp32s3 --port /dev/ttyACM0 read-flash 0 ALL papercolor-factory-16MB.bin
```

### Bug 1 — "Packet content transfer stopped", every time

The full read died partway through. Retrying gave the *same* error. The
diagnostic move was to **shrink the problem**:

1. A 64 KB read → worked. So reading works in principle.
2. Sixteen 1 MB reads → 14 worked, **2 failed — at exactly the same addresses
   on every retry** (`0x10d000`, `0xa0f000`).

Same address every time = **not** flaky USB (that would be random). Something
about the *bytes in those blocks* trips esptool's fast "stub" reader over the
S3's built-in USB. Retrying can never fix a deterministic failure.

**Fix:** esptool has a second, slower reader built into the chip's ROM
(`--no-stub`) that packages data differently. We read fast everywhere, and
switched to the ROM reader for 8 KB around each failure. A small script did
this automatically and found six bad blocks in total (`0x10d000`, `0x140000`,
`0x143000`, `0x17b000`, `0x184000`, `0xa0f000`).

**Verify — don't assume.** Stitching 20-odd pieces together is exactly where a
silent mistake hides, so we asked the *chip* to check our file:

```bash
esptool --chip esp32s3 --port /dev/ttyACM0 verify-flash 0 papercolor-factory-16MB.bin
# Verification successful (digest matched).
```

The backup lives in `backup/` (read-only, with its SHA-256). Restore with:

```bash
esptool --chip esp32s3 --port /dev/ttyACM0 write-flash 0 backup/papercolor-factory-16MB.bin
```

The factory layout, decoded from the backup's partition table: `nvs` (24 KB),
`phy_init`, a 10 MB `factory` app, and a 6 MB FAT `storage` partition (the
demo's assets).

> **Lesson:** a *deterministic* failure is information. Find the pattern
> (bisect by size, then by address), and change *method*, not *attempt count*.

---

## Module 2 — Stage 1: "Hello, serial"

Prove compile → flash → read before any peripheral.

### `platformio.ini` — the parts that matter

```ini
platform  = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.311/platform-espressif32.zip
board     = esp32-s3-devkitc-1
board_build.flash_size  = 16MB
board_build.partitions  = default_16MB.csv
board_build.arduino.memory_type = qio_opi   ; the "R8" = 8 MB OCTAL PSRAM
build_flags =
	-DBOARD_HAS_PSRAM
	-DARDUINO_USB_MODE=1        ; Serial over the USB-C port itself
	-DARDUINO_USB_CDC_ON_BOOT=1
```

### Bug 2 — the newest platform needs a newer PlatformIO

```
IncompatiblePlatform: ... depends on PlatformIO Core >=6.2.0.
```

The newest pioarduino release (`55.03.312-1`) requires PlatformIO Core 6.2; we
have 6.1.19. Two fixes: `pio upgrade` (changes the tool *every* project uses),
or pin the previous release `55.03.311`. We pinned — the smallest change that
affects only this project.

### Pins as typed constants (`include/board_pins.h`)

```cpp
constexpr uint8_t PIN_AUDIO_PWR_EN = 45;
constexpr uint8_t PIN_SPK_EN       = 46;
```

> **C++ for Pythonistas:** `constexpr` is like a Python module-level constant
> (`PIN_SPK_EN = 46`) except the compiler *enforces* it can't change, knows its
> type (`uint8_t` = an unsigned 8-bit integer, 0–255) and costs nothing at
> runtime. Prefer it to `#define`, which is blind text substitution (gotcha 37).

### Result

```
PaperColor Stage 1: hello, serial
Chip: ESP32-S3 rev 2, 2 cores @ 240 MHz
Flash: 16 MB
PSRAM: found, 8192 KB total
tick 0, tick 1, ...
```

PSRAM found matters: the display library refuses to run without it.

---

## Module 3 — Stage 2: First light on e-paper

### How e-paper is different

An LCD is a lit window you repaint 60 times a second. E-paper is **tiny
charged particles physically moved** by voltage; once moved they stay put with
**zero power**. Consequences:

- One full refresh takes about **16 seconds** (measured), with visible flashing
  while the particles are shaken into place.
- No backlight → no "backlight pin" bug (gotcha 4) — but **the panel's power is
  switched by the PMIC**, the same class of trap in a new place. M5Unified
  handles it.
- The image survives reboots and even reflashing. After Stage 1 the screen
  still showed the *factory* picture — nothing had told it to change.

### Where the picture lives

M5GFX draws into a **frame buffer** in PSRAM: 400 × 600 pixels × 3 bytes
(RGB888) = **720 KB**. Only when you call `display()` does the driver convert
each row to the six inks, pack them **2 pixels per byte**, send them over SPI,
and trigger the refresh. That is why PSRAM is mandatory.

### Trap 1 — auto-display: one refresh *per drawing call*

M5GFX's frame-buffer panels default to **auto-display**: every `fillRect`,
every `drawString`, ends with a refresh. Fine for an LCD; on this panel our
first-light screen (~15 drawing calls) would have taken **about four minutes**
of continuous flashing. The fix is the core e-paper habit:

```cpp
M5.Display.setAutoDisplay(false);   // draw into the buffer silently...
// ... all the drawing ...
M5.Display.display();               // ...then ONE refresh
M5.Display.waitDisplay();
```

### Trap 2 — the hidden "clear" refresh

`M5.begin()` normally clears the screen to white — another full 16 s refresh
before your own. We switch it off:

```cpp
auto cfg = M5.config();
cfg.internal_spk  = false;   // silent: never set up the speaker
cfg.internal_mic  = false;
cfg.clear_display = false;   // skip the wasted clear-to-white refresh
M5.begin(cfg);
```

> **C++ for Pythonistas:** `auto cfg = M5.config();` — `auto` lets the
> compiler work out the type, like Python's untyped assignment. `cfg` is a
> plain struct of settings, much like a dataclass you tweak before passing in.

### Drawing exactly the six inks

The driver converts any RGB colour to its nearest ink. Rather than hope, we
used the **exact RGB values from the driver's own palette** (`epd_palette[]` in
`Panel_ED2208.inl`), so each swatch is a pure ink:

| Ink | RGB the driver expects |
|---|---|
| BLACK | 0, 0, 0 |
| WHITE | 255, 255, 255 |
| YELLOW | 255, 243, 56 |
| RED | 191, 0, 0 |
| BLUE | 100, 64, 255 |
| GREEN | 67, 138, 28 |

We also chose `setEpdMode(epd_mode_t::epd_fastest)` — nearest-ink, **no
dithering**. Dithering mixes dots of two inks to fake in-between colours (great
for photos); on flat palette colours it would only add speckle.

### Result (confirmed by eye)

Title, six solid, correctly-labelled swatches, a red triangle marking the
top-left, and "400 x 600" — portrait. Refresh: **16,160 ms**.

---

## Module 4 — Stage 3a: Probe the buttons before using them

Gotchas 22 and 27 taught us: vendor pin lists are hypotheses, and an
incomplete probe is its own trap. The probe (`stages/stage3a_button_probe.cpp`)
asked three questions per pin:

```cpp
pinMode(p.pin, INPUT_PULLDOWN); int vDown = digitalRead(p.pin);
pinMode(p.pin, INPUT_PULLUP);   int vUp   = digitalRead(p.pin);
pinMode(p.pin, INPUT);          p.level   = digitalRead(p.pin);
```

If a pin still reads **HIGH while our internal pull-*down* tries to drag it
LOW**, a *stronger external* pull-up resistor must be on the board.

### Result

```
A  G10  pulldown:1 pullup:1 plain:1  -> EXTERNAL pull-up (idle HIGH)
B  G9   pulldown:1 pullup:1 plain:1  -> EXTERNAL pull-up (idle HIGH)
C  G1   pulldown:1 pullup:1 plain:1  -> EXTERNAL pull-up (idle HIGH)
[133.532 s] A  G10  1 -> 0   ... (tap 251 ms, hold 2264 ms)
```

- **Three real buttons** (the "missing" third was top-centre).
- **Active LOW**, **external pull-ups**, clean single edges, **no rockers**
  (pressing other directions produced nothing).
- External pull-ups are good news for later: they keep working in deep sleep,
  when the chip's internal ones lose power (gotcha 22).

---

## Module 5 — Stage 3b: The microSD slot

### Bug 3 — the mount that hung forever

The probe followed the T-Display recipe (gotcha 25): its own `SPIClass` on the
display's SPI host.

```cpp
SPIClass sdSPI(FSPI);                 // T-Display habit
sdSPI.begin(15, 14, 13, 47);
SD.begin(47, sdSPI, 20000000);        // ...never returned
```

The boot log held the clue we almost skipped:

```
[E][esp32-hal-cpu.c:143] addApbChangeCallback(): duplicate func=...
```

"Duplicate" — something had been set up twice. Reading M5GFX's source
(`platforms/esp32/common.inl`) settled it: **M5GFX itself calls
`SPI.begin(...)` on Arduino's *global* `SPI` object** and shares its bus
handle. Our second `SPIClass` on the same hardware meant **two owners of one
bus, each with its own lock** — and the mount deadlocked.

**Fix:** don't create a bus at all — use the global one M5GFX already owns,
exactly as M5Stack's own examples do:

```cpp
// M5.begin() first: it powers the SD slot (PMIC) and starts the shared SPI bus
SD.begin(PIN_SD_CS, SPI, 20000000);
```

> **Lesson:** "share the bus" has no single recipe — it depends on what the
> *other* driver does. Arduino_GFX didn't touch the global `SPI` (so we made
> our own); M5GFX does (so we must reuse it). The answer was in the driver's
> source, not in our playbook (gotcha 26: check what a thing is derived from).

### Result

```
Mounted at 20000 kHz  type:SDHC  card:61120 MB  fs used:7 / 31 MB
read back: "hello from PaperColor, millis=285"   delete test file: ok
```

The card in the slot was a **Raspberry Pi 4 boot card** — the 31 MB figure is
its small FAT boot partition, not the whole 64 GB card. The write/read/delete
round-trip proved the slot works; after that we made the probe **read-only**,
because it was someone else's working card. (Lesson in itself: probes that
write should ask first.)

### Bug 4 (hiding) — a freshly inserted card is slow to wake

On a re-probe right after reinsertion, 20 MHz and 10 MHz failed (`Card Failed!
cmd: 0x00` — no reply at all) and it mounted at **4 MHz**; three seconds later
it mounted at 20 MHz again. A just-inserted card needs a moment to settle. Code
that mounts immediately and *remembers* the first speed that worked would run
5× slower forever. Mount after a settle delay, or retry fast later.

---

## Module 6 — The docs said G1 was card-detect. It isn't.

The spec lists G1 as **both** Button C and SD card-detect. We watched G1 raw
through card removal and reinsertion: it **never changed**. G1 is just the
button, and this board has **no card-detect line** — the only way to know a
card is present is to try mounting it.

> **Lesson:** a contradiction in documentation is a test waiting to be run.
> Two minutes with a probe beat any amount of reasoning about which line of
> the doc to believe.

---

## Module 7 — Stage 3c: Buttons + LEDs — the e-paper interaction pattern

A 16-second refresh rules out instant on-screen feedback. The board's two RGB
LEDs fill the gap:

| Output | Speed | Job |
|---|---|---|
| LED 0 | instant | **preview** the colour you're choosing |
| LED 1 | instant | **status**: green = ready, red = refreshing |
| E-paper | ~16 s | **results**: redraw once, on purpose |

- **A / B** step the choice backward/forward (wrapping); LED 0 changes at once.
- **C** commits: the screen is redrawn with a big panel in that colour.
- Pressing C when that colour is **already showing** does nothing — a
  pointless refresh costs 16 s, power, and panel wear.

### Same colour name, two RGB values

```cpp
struct Choice { const char* name; uint8_t ir, ig, ib; uint8_t lr, lg, lb; };
{ "BLUE",   100,  64, 255,     0,   0, 255 },
```

The panel's "blue ink" value (100, 64, 255) looks **purple** on an LED, and LED
"black" is simply *off*. Each medium gets its own value. (This mirrors real
colour management: a colour *name* maps to different numbers per device.)

> **C++ for Pythonistas:** `struct Choice {...}` is like a tiny `@dataclass`
> (or a `namedtuple`). `static constexpr Choice CHOICES[] = {...}` is a
> constant table baked into flash. `sizeof(CHOICES) / sizeof(CHOICES[0])`
> is how C++ computes `len(CHOICES)` for a plain array.
> `(selected + N - 1) % N` wraps backwards — Python's `%` already handles
> negatives, but C++'s `%` of a negative number stays negative, so we add `N`
> first.

### Debounced buttons for free

```cpp
M5.update();                       // poll + debounce every loop
if (M5.BtnA.wasPressed()) { ... }  // true for exactly ONE loop per press
```

Compare the T-Display's hand-rolled tap logic (gotchas 12–13): M5Unified does
the edge detection and debounce for you.

### Design decision — presses during a refresh are lost (accepted)

`display()` **blocks for the whole refresh** (it waits on the panel's BUSY
pin), so `M5.update()` doesn't run and presses in that window vanish — which
we confirmed by hand. Options were: accept it; latch presses with interrupts;
or move the refresh to the idle second core in a FreeRTOS task (gotcha 38).
**We accepted it for now**: the red status LED already says "wait", and the
e-paper pattern is "draw once, then sleep" — rarely "draw while typing".

### Result

```
commit #1: GREEN - refreshing...   refresh done in 16160 ms
C: already on screen - no refresh
commit #2: RED - refreshing...     refresh done in 16160 ms
```

Every refresh measured so far — six swatches, a green screen, a red screen —
took **exactly 16,160 ms**, regardless of content. M5Stack quotes "15–30 s
depending on colour complexity"; with this driver the time looks fixed, which
makes it easy to plan around.

---

## Debugging summary — what actually went wrong

| # | Symptom | Cause | Fix |
|---|---|---|---|
| 1 | Flash read fails, same addresses each time | esptool stub reader vs certain block contents over native USB | ROM reader (`--no-stub`) around bad blocks, then `verify-flash` |
| 2 | `IncompatiblePlatform ... Core >=6.2.0` | newest pioarduino needs newer PlatformIO | pin `55.03.311` |
| 3 | SD mount hangs; `addApbChangeCallback(): duplicate` | second `SPIClass` on a bus M5GFX already owns via global `SPI` | use global `SPI` after `M5.begin()` |
| 4 | Re-inserted card fails at 20/10 MHz, mounts at 4 MHz | card not settled after insertion | settle/retry; don't lock in the slow speed |
| — | Editor red squiggles (`Arduino.h` not found) | clangd vs Xtensa GCC flags | `pio run -t compiledb` + `.clangd` stripping the flags (build unaffected) |

Plus one "doc bug": G1 is not card-detect.

---

## What you built and learned

- ✅ Verified full-flash backup; one-command restore
- ✅ Silent firmware (audio held off from the first line of `setup()`)
- ✅ E-paper first light: six exact inks, one refresh, measured at 16.16 s
- ✅ Three buttons probed: external pull-ups, active LOW, no rockers
- ✅ microSD at 20 MHz on the shared bus — done the *M5GFX* way
- ✅ LED-for-feedback / e-paper-for-results interaction pattern
- ✅ Two documentation claims tested; one (G1 card-detect) disproved

Stage snapshots are in `stages/`: `stage2_first_light.cpp`,
`stage3a_button_probe.cpp`, `stage3b_sd_probe.cpp`, `stage3c_buttons_led.cpp`.
(`src/main.cpp` moves on with later lessons — see `docs/PLAN.md` for what it holds now.)

---

## Command cheat-sheet

```bash
pio run                                  # build
pio run -t upload                        # build + flash
pio run -t compiledb                     # compile_commands.json for the editor
pio device monitor                       # serial (in your own terminal)

# flash backup / verify / restore
esptool --chip esp32s3 --port /dev/ttyACM0 read-flash 0 ALL out.bin
esptool --chip esp32s3 --port /dev/ttyACM0 --no-stub read-flash ADDR LEN out.bin  # slow ROM reader
esptool --chip esp32s3 --port /dev/ttyACM0 verify-flash 0 backup/papercolor-factory-16MB.bin
esptool --chip esp32s3 --port /dev/ttyACM0 write-flash 0 backup/papercolor-factory-16MB.bin

# recovery if the board won't accept an upload: hold the side button ~3 s (download mode)
```

---

## Glossary

- **E-paper / E Ink Spectra 6** — a reflective display of charged pigment
  particles; holds its image with no power; six inks; ~16 s refresh.
- **Frame buffer** — the full picture held in memory (here 720 KB in PSRAM);
  drawing changes the buffer, `display()` sends it to the glass.
- **Auto-display** — M5GFX mode that refreshes after every drawing call;
  turn **off** on e-paper.
- **Dithering** — mixing dots of available inks to imitate colours the panel
  can't show; off (`epd_fastest`) for flat colours.
- **BUSY pin** — the panel's "I'm still refreshing" signal; `waitDisplay()`
  waits on it.
- **PMIC** — power-management IC (M5PM1); here it switches power to the
  display and SD slot.
- **Stub flasher** — the fast helper program esptool uploads into RAM; the
  **ROM loader** (`--no-stub`) is the slower one built into the chip.
- **Pull-up (external / internal)** — a resistor holding a pin HIGH when
  nothing drives it; external ones are on the board and survive deep sleep.
- **Active LOW** — "pressed" reads 0.
- **Card-detect** — a switch in an SD slot that signals card presence; this
  board has none.
- **RMT** — the ESP32 peripheral that generates the precise pulse timing
  addressable RGB LEDs need.

---

## Next lesson

**Lesson 02 — Inside the driver:** read `Panel_ED2208` together — the vendor
init bytes, the BUSY handshake, packing two pixels per byte, the palette
lookup and the dithering algorithms — then compare `epd_fastest` and
`epd_quality` on a gradient to *see* dithering. After that: the SHT40 sensor,
the RTC, and a deep-sleep dashboard — the job this board was built for.

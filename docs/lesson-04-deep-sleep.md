# Lesson 04 — Deep Sleep, and Measuring What It Really Costs

**Project:** M5Stack PaperColor (SKU C151) in C++ with PlatformIO
**Goal of this lesson:** Turn Lesson 03's always-on dashboard into one that
**deep-sleeps** between short wakes — the job e-paper is made for — then find out
what it really costs with an inline USB meter.

> Audience note: Python / CircuitPython background, learning C++.
> **Status:** Parts 1–2 done (deep-sleep dashboard; power bench). The finding that
> changes the plan: a USB meter **cannot see this board's own load** while it's
> plugged in.

---

## Learning objectives

1. Structure a program as **wake → decide → act → sleep**, where every wake is a reboot.
2. Keep state across deep sleep with `RTC_DATA_ATTR`, and detect a cold boot.
3. Choose wake sources (timer + buttons via EXT1) and hold non-RTC pins through sleep.
4. Keep a board flashable when it sleeps most of the time.
5. Use a **power bench** to attribute current to individual supplies.
6. Recognise when an instrument **can't** measure what you want — and find a
   measurement that settles it.

---

## Research first (the vendor library decides the design)

Read before writing (`M5Unified/src/utility/Power_Class.inl`):

- **`M5.Power.timerSleep()` on this board is ordinary ESP32 deep sleep.** It sets an
  RTC alarm and a timer wake, then `esp_deep_sleep_start()`. A true **power-chip-off**
  (`M5pm1.powerOff()`) happens only **without** a timer — wake is then by the power
  button only.
- **Correction to the skill (gotcha 19):** it says "the S3 dropped EXT0 — use EXT1".
  ESP-IDF's `soc_caps.h` for the S3 says `SOC_PM_SUPPORT_EXT0_WAKEUP (1)` — the S3
  supports **both**. We use **EXT1** anyway: one mask watches all three buttons.
- The three buttons (G10, G9, G1) have **external pull-ups** (Lesson 01 probe) and are
  RTC-capable (≤ G21), so EXT1 "any low" needs no internal pull-ups in sleep.

---

## Part 1 — The deep-sleep dashboard (`stages/lesson4_deep_sleep.cpp`)

### Wake → decide → act → sleep

```
every wake (timer every 2 min, or any button):
    read SHT40 + RTC
    refresh if: button | 10 min since last refresh | T moved ≥ 0.5 °C
    otherwise: don't touch the display at all
    deep sleep
```

**Deep sleep is a reboot, not a pause.** `setup()` runs on every wake; normal
globals start fresh. What must survive goes in **RTC memory**:

```cpp
RTC_DATA_ATTR static uint32_t rtcMagic;            // "is RTC memory valid?"
RTC_DATA_ATTR static float    shownT, minT, maxT;
RTC_DATA_ATTR static time_t   lastRefreshAt;
RTC_DATA_ATTR static LogEntry logBuf[64];          // ~1.5 KB of ~8 KB
```

> **C++ for Pythonistas:** `RTC_DATA_ATTR` is an *attribute* telling the linker to
> place the variable in the small memory that stays powered in deep sleep. There's
> no Python equivalent — the closest idea is pickling state to disk before exiting.

A **magic number** detects garbage: on a cold boot (power-on/reset) RTC memory is
undefined, so we reset everything unless `rtcMagic == MAGIC`.

### Details that matter

- **Never `clear_display`** on wake — the screen already shows our last frame; M5's
  default clear would cost a 16 s refresh every wake.
- **Time:** the ESP32's sleep clock runs on an internal RC oscillator and drifts, so
  every wake re-reads the RX8130 (no edge wait — HH:MM doesn't need it), then
  re-applies the TZ (Lesson 03's stale-pointer workaround).
- **Hold non-RTC pins:** G45/G46 (audio power, speaker amp) would float in deep
  sleep. `gpio_hold_en()` + `gpio_deep_sleep_hold_en()` latch them LOW; released
  first thing in `setup()`.
- **Don't sleep with a button held** — EXT1 "any low" would wake instantly.
- **Flash window:** after a cold boot the board stays awake ~20 s (plus the 16 s
  refresh) so uploads work. Timer wakes are ~1 s.

### Result — and an unexpected instrument

```
[08:35:39] sleeping 120 s (awake 36377 ms)       ← cold boot: refresh + flash window
```

After that, **no serial at all** — a 1 s wake ends before the host finishes setting up
the USB serial port. The **host's kernel log** became the instrument:

```
08:37:38  usb 3-2: new full-speed USB device number 8     ← wake
08:39:38  usb 3-2: new full-speed USB device number 9     ← wake
08:41:38  usb 3-2: new full-speed USB device number 10    ← wake
08:45:38  ... ttyACM0: USB ACM device / 08:45:54 disconnect ← scheduled refresh (16 s awake)
```

Wakes every **120 s exactly**; the 10-minute refresh happened on schedule. (A 1 s wake
is also shorter than a cheap meter's ~1 s averaging — you'll miss the blip.)

`tools/follow_serial.py` follows the board across sleeps (reopens the port each time).
Flashing a sleeping board: start an upload that waits for the port, then press a
button (the 16 s refresh is a long enough window); fallback: hold the side button
~3 s for download mode.

---

## Part 2 — The power bench (`stages/lesson4_power_bench.cpp`)

### The puzzle

With the USB meter inline, the "sleeping" dashboard read a steady **24 mA**. The
ESP32-S3 itself sleeps at ~10 µA — so something else was on. Pulling the SD card
changed nothing.

### Method (gotcha 20)

A firmware mode that **holds one power state still**, announcing each step on the
screen first, then switching **one more supply off** — cumulatively, so the
*difference* between neighbouring steps is that supply's cost. Advance by button.

M5Unified exposes the power chip as `M5.Power.M5pm1`: `setExtOutput()` (Grove 5 V
boost), `setLDOOutput()` (RGB LED supply), `setGPIOOutput(gpio3/gpio0)` (SD / panel
power), `powerOff()`.

### Run 1

| Step | State | Meter |
|---|---|---|
| 0 | awake | 24 mA |
| 1 | deep sleep | 24 mA |
| 2 | + LED supply off | 24 mA |
| 3 | **+ Grove 5 V off** | **0.00000 A** |
| 4 | + SD power off | 0.00000 A |
| 5 | + panel power off | 0.00000 A |
| 6 | power chip OFF | 220 → 120 mA, falling |

1. **The 24 mA was the Grove 5 V output** — on by default (`grove5V 1` in the log),
   with **nothing plugged in**. A boost converter idling with no load should draw
   well under 1 mA; why this one draws 24 mA is unexplained (schematic needed). The
   fix doesn't depend on the why: **switch it off.**
2. **"Charging: no" on the screen was worthless.** `M5PM1_Class::isCharging()` is a
   *"permanent stub returning false"* — the PM1 has no charging-status register. It
   could only ever say "no". I displayed it without checking its source: the **fifth
   instrument** in this project that couldn't disagree (gotchas 26, 29).
3. **An impossible pair:** step 0 (awake) = step 1 (asleep) = 24 mA. An awake ESP32
   draws tens of mA — so if the board ran from USB, step 0 would be far higher.
4. **Step 6 = charging restarting:** once the power chip cut the system, USB current
   jumped and **tapered** — a lithium top-off.

### Run 2 — the decisive step

Bench v2 added **step 1: awake, Grove 5 V off**. It can only come out two ways:
~0 → the board runs from its battery; 20–40 mA → it runs from USB.

| Step | State | Meter |
|---|---|---|
| 0 | awake (right after replugging) | 168 → 150 mA, falling … settled **24 mA** |
| 1 | **awake, Grove 5 V off** | **0.00000 A** |

The serial log adds an independent witness: battery **4.23 V** at step 0 (charging
lifts the terminal voltage — gotcha 16) vs **4.18 V** at step 1.

### Conclusions

- **With USB connected and charging finished, the board runs from its battery.** USB
  supplies only charging and the Grove 5 V. **This USB meter cannot measure the
  board's own consumption** — the 0.00000 A in sleep steps meant "not visible", not
  "zero".
- **The charger restarts on every plug-in and on power-chip power-off** (168 mA and
  220 mA tapering top-offs on a full battery) — so every measurement must wait for the
  taper to finish.
- **The real-world bug:** M5Unified's defaults leave the **Grove 5 V on**. On battery,
  that would have drained the "deep-sleep" dashboard in days, not months. *Audit every
  supply before sleeping — vendor defaults are tuned for "works out of the box", not
  for battery life.*

*(Part 3 — how to measure the real consumption, and the improved dashboard — next.)*

---

## Files

| File | What |
|---|---|
| `stages/lesson4_deep_sleep.cpp` | deep-sleep dashboard (timer + EXT1 wake, RTC memory) |
| `stages/lesson4_power_bench.cpp` | power bench v2 (awake/Grove-off step, no fake charging line) |
| `tools/follow_serial.py` | follow serial output across deep sleeps |

## Glossary

- **Deep sleep** — almost everything powered down; wake = reboot.
- **RTC memory / `RTC_DATA_ATTR`** — the few KB that stay powered in deep sleep.
- **EXT0 / EXT1** — wake on one RTC pin / on any (or all) of a set of RTC pins.
- **GPIO hold** — latch a pin's level so it doesn't float while the chip sleeps.
- **Power path** — how a charger chip routes USB and battery power to the system.
- **Top-off / taper** — the end of a lithium charge: current falls as the battery fills.
- **Stub** — a function that exists but doesn't do the real work (returns a fixed value).

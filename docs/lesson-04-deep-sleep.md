# Lesson 04 — Deep Sleep, and Measuring What It Really Costs

**Project:** M5Stack PaperColor (SKU C151) in C++ with PlatformIO
**Goal of this lesson:** Turn Lesson 03's always-on dashboard into one that
**deep-sleeps** between short wakes — the job e-paper is made for — then find out
what it really costs with an inline USB meter.

> Audience note: Python / CircuitPython background, learning C++.
> **Status:** Parts 1–5 done; Part 5's 24 h battery test (L1 standby) running.
> Story so far: deep sleep (L2) drained **~38 mA average** on battery; every switchable
> rail was verified off; the vendor schematic showed why, and the fix is the power
> chip's **L1 standby** (everything off, PMIC timer powers the board back on).

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

## Part 3 — Dashboard v2 and the 24-hour battery test (`stages/lesson4_deep_sleep_v2.cpp`)

v2 switched off **Grove 5 V, LED supply, SD power and panel power** before every sleep,
kept an **hourly battery log** in RTC memory, and opened a **30 s service window** on a
button wake (plugging USB into a sleeping board doesn't reboot it) where `LOG` dumps
the logs. `tools/pull_log.py` waits for that window and saves the CSV.

Verified first: a timer wake takes only **217 ms**; the panel survives being powered
off between wakes; the log pull works.

### Result — far worse than estimated (`docs/data/battery-2026-10-05.csv`)

| | |
|---|---|
| Duration on battery | 22.6 h (23 hourly points) |
| Voltage | 4130 → 3692 mV, a **straight line, −18.8 mV/h** (residual ±22 mV) |
| Charge used (typical LiPo curve, ±50 %) | ~93 % → ~24 %, **~870 mAh ⇒ ~38 mA average** |
| Estimate beforehand | refreshes ~1 mA + wakes ~0.1 mA avg ⇒ **~1.4 mA** |

Refreshes (142, on schedule) and wakes (700) explain only ~1–2 mA: **~35 mA flowed
while "asleep"**. A straight line means a constant load, not an event. (Bonus: hourly
entries were 61.6 min apart → the 120 s ESP32 sleep timer actually ran **123.3 s**, its
RC oscillator ~2.7 % slow — the reason to re-read the RTC chip every wake.)

## Part 4 — Read-back check and the schematic

### Read back, don't assume

v2.1 read the power chip's registers **after** switching rails off, just before
sleeping, and kept them for the next service window:

```
PWR_CFG 0x13 → LED_EN 1  BOOST_5V 0  LDO_3V3(RGB) 0  DCDC_3V3 1  CHG_EN 1
GPIO_OUT 0x00 → G0 (e-paper power) 0   G3 (SD power) 0
```

Every rail we switch **was** off (the suspicion that a write silently failed was wrong).
`DCDC_3V3 1` = the main 3.3 V converter stays on in deep sleep — by design. `LED_EN`
goes to a debug test point (`PY_SWD`), not a load. 5V-out read 4.87 V only because USB
feeds that net through the Grove two-way circuit.

### What the vendor schematic says (`docs/ref/C151-PaperColor-schematic-V0.5.pdf`)

- **The board always runs from its battery:** USB-C → IP2315 *charger* → VBAT; the
  system rail is VBAT through a 0 Ω link. Nothing moves the load onto USB — that's why
  the USB meter was blind (G50), now confirmed from the hardware.
- **Vendor power modes** (page 1, "Power Network"):

| Mode | Powered |
|---|---|
| L0 Shipping | nothing |
| **L1 Standby** | power chip + RTC only — the **92.53 µA "standby"** |
| **L2 DeepSleep** | + main 3.3 V converter (JW5712): ESP32, SHT40, keys, switches for LEDs/SD/e-paper |
| L3A / L3B | active |

Our deep-sleep dashboard lived in **L2**. The ~35 mA is on something that stays powered
there and can't be switched separately; with the meter blind on battery it can't be
isolated further from USB. L1 removes the whole L2 domain — the fix, and a test: if the
drain collapses in L1, the load was on the main 3.3 V rail.

### The power chip can switch the board back on (`docs/ref/M5PM1_Datasheet_EN.pdf`)

- `TIM_CNT` 0x38–0x3B (seconds, LSB first), `TIM_CFG` 0x3C = `0x08` ARM | `0b011`
  "**system power on**", `TIM_KEY` 0x3D = `0xA5`, then `SYS_CMD` 0x0C = `0xA1` (power
  off) — the exact sequence from M5Stack's own M5PM1 library.
- `RTC_MEM` 0xA0–0xBF: **32 bytes retained across ESP32 power-off** (not across loss of
  the PMIC's own power).
- `WAKE_SRC` 0x05: TIM / VIN (USB plugged) / PWRBTN flags, **write-0-to-clear**. M5Unified
  doesn't clear it on PaperColor (it does on ToughC5/PaperMono) — read it after `begin()`.
- From L1 only the **power button**, the RTC interrupt or the PMIC timer wake the board —
  buttons A/B/C only work from L2 (accepted trade-off).

## Part 5 — L1 standby dashboard (`stages/lesson4_standby_l1.cpp`)

Every power-on: read sensor + RTC → maybe refresh → save state → arm PMIC timer
(120 s, "power on") → PMIC power-off. The ESP32 is completely off in between, so:

- **State** (wakes, refreshes, last refresh/battery-log times, shown/min/max °C, last
  awake ms) is a 30-byte packed struct in the PMIC's `RTC_MEM`;
  `static_assert(sizeof(State) <= 32)` makes the compiler enforce the limit.
- **Battery log** goes to **NVS flash** (`Preferences`), hourly — 24 writes/day.
- **Why are we running?** `esp_reset_reason()`: `ESP_RST_POWERON` (1) = the PMIC
  switched us on; `ESP_RST_USB` (11) = reset after flashing → fresh state + 20 s flash
  window. Then `WAKE_SRC`: TIM = routine; PWRBTN/VIN = refresh + 30 s service window.
- If the power-off doesn't happen, fall back to ESP32 deep sleep.

> **C++ for Pythonistas:** `struct __attribute__((packed))` removes the padding the
> compiler would normally insert between fields (like `struct.pack('<HII...')` vs a
> padded C struct) so the bytes fit the 32-byte RAM exactly. `static_assert` is a check
> done at *compile* time — the build fails if the struct grows too big.

### First run

```
09:29:54 wake #1 (fresh, WAKE_SRC 0x02, reset 11): T 20.27 batt 3.99 V refreshed   ← after flashing
09:30:14 power-off for 120 s ... E BOD: Brownout detector was triggered
09:32:31 wake #2 (timer, WAKE_SRC 0x01, reset 1): T 20.89 ... refreshed (T moved 0.62 °C)
kernel:  09:34:31 connect → 09:34:32 disconnect                                     ← 1 s routine wake
```

- The PMIC really powers the board **off** and back **on** — and its timer is exact:
  power-off 09:30:14 → power-on 09:32:14 (**120 s**, vs the ESP32 RC timer's 123.3 s).
- State survived the power-off (wake #2) via `RTC_MEM`.
- The **brownout message** is the ESP32 noticing its supply collapse as the PMIC cuts
  it — expected for a deliberate power-off, harmless here.
- A routine wake (no refresh) is ~1 s from USB connect to disconnect: a full boot costs
  more than a 217 ms deep-sleep wake, still small next to a 16 s refresh.

### 24 h result (`docs/data/battery-L1-2026-10-06.csv`)

| | L2 deep sleep | **L1 standby** |
|---|---|---|
| Voltage slope | −18.8 mV/h | **−1.89 mV/h** (17 points, ±3.7 mV) |
| Average current (rough, typical LiPo curve) | ~38 mA | **~4 mA** |
| Battery life (rough) | ~1 day | **~2 weeks** |

**~10× less drain** — the A/B test answered the Part 4 question: the ~35 mA lived on
the main 3.3 V rail, which L1 switches off.

**But the board stopped waking at ~03:00** — no hourly entries for **8.7 h**, until the
USB plug-in woke it. The display froze on its last image and the battery barely moved:
exactly what was seen. ~520 wakes had worked; on one, the PMIC powered off **without a
timer armed**. v1 ignored every PMIC write's return value — **gotcha 29 in my own
code**: a write that isn't read back isn't known to have happened.

## Part 6 — Standby v2: never power off without a verified timer (`stages/lesson4_standby_l1_v2.cpp`)

- `armTimer()` writes `TIM_CNT`/`TIM_CFG`/`TIM_KEY`, then **reads `TIM_CNT` and
  `TIM_CFG` back** (count within 3 s of the request — it may already be ticking — and
  `ARM | power-on` set). Up to 3 attempts.
- **Only a verified timer allows the PMIC power-off.** Otherwise: stop the timer and
  ESP32 deep-sleep for this cycle (more power once, never a dead board).
- Diagnostics in the 32-byte state (`armRetries`, `fallbacks`, now exactly 32 bytes —
  `static_assert` still holds) plus a power-off-failure count in NVS; `LOG` reports them,
  and the screen shows `arm rN fN`.
- **Second bug, found while planning the fallback:** a deep-sleep wake has reset reason
  `ESP_RST_DEEPSLEEP`, which v1 treated as a dev boot — wiping state *and* the battery
  log. Now `POWERON` or `DEEPSLEEP` continue the run.

First run: `power-off for 120 s (... timer verified)` — the PMIC does return the count
it was given, so read-back is a valid check. Wakes continued every ~2 min.

*(24 h test of v2 — next: are there retries/fallbacks, and does it keep waking?)*

---

## Files

| File | What |
|---|---|
| `stages/lesson4_deep_sleep.cpp` | deep-sleep dashboard (timer + EXT1 wake, RTC memory) |
| `stages/lesson4_power_bench.cpp` | power bench v2 (awake/Grove-off step, no fake charging line) |
| `tools/follow_serial.py` | follow serial output across deep sleeps |
| `stages/lesson4_deep_sleep_v2.cpp` | v2.1: rails off + register read-back |
| `stages/lesson4_standby_l1.cpp` | L1 standby: PMIC timer power-on, state in PMIC RTC_MEM |
| `tools/pull_log.py` | pull logs during a service window |

## Glossary

- **Deep sleep** — almost everything powered down; wake = reboot.
- **RTC memory / `RTC_DATA_ATTR`** — the few KB that stay powered in deep sleep.
- **EXT0 / EXT1** — wake on one RTC pin / on any (or all) of a set of RTC pins.
- **GPIO hold** — latch a pin's level so it doesn't float while the chip sleeps.
- **Power path** — how a charger chip routes USB and battery power to the system.
- **Top-off / taper** — the end of a lithium charge: current falls as the battery fills.
- **Stub** — a function that exists but doesn't do the real work (returns a fixed value).

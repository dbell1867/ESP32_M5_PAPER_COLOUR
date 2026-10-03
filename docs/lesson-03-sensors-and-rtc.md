# Lesson 03 — Sensors and the Real-Time Clock

**Project:** M5Stack PaperColor (SKU C151) in C++ with PlatformIO
**Goal of this lesson:** Bring up the board's internal I²C devices — survey the
bus, write our own SHT40 temperature/humidity driver, and get the RX8130CE
real-time clock keeping correct UK time — then combine them on the e-paper.

> Audience note: Python / CircuitPython background, learning C++.
> **Status:** Parts 1–3 done; Part 4 (dashboard + cold-refresh experiment) next.

---

## Learning objectives

1. Find out **who owns a bus** before touching it, and use the owner's API.
2. Make a bus scan that can *disagree* — and spot a C++ uninitialised-memory bug.
3. Write an I²C driver from a datasheet: command, wait, read, **CRC**, convert.
4. Tell **precision** from **accuracy**, and know what `delay()` really guarantees.
5. Read and set an RTC; understand **BCD**, store **UTC**, display local time.
6. Find a real library bug (a stale `char*`) and work around it without editing
   the library.
7. Synchronise two clocks properly ("sync on the edge").

---

## Part 1 — Who is on the internal I²C bus?

### Who owns the bus?

Reading M5Unified's setup (`M5Unified.inl:2140–2160`):

| Bus | Pins | I²C port | Used for |
|---|---|---|---|
| `M5.In_I2C` (internal) | SDA G3 / SCL G2 | **1** | RTC, SHT40, PMIC, audio chips |
| `M5.Ex_I2C` (Grove) | G4 / G5 | **0** | external units |
| Arduino `Wire` | — | **0** | (same port as Ex_I2C!) |

So the instinctive `Wire.begin(3, 2)` would re-route the internal bus's pins onto
port 0 and **take them from M5Unified** — the RTC and power manager would silently
stop answering it (gotcha 28's port trap). Rule: **everything on the internal bus
goes through `M5.In_I2C`.** Confirmed at runtime: `In_I2C port 1, SDA G3, SCL G2`.

### A scan that can disagree (`stages/lesson3_i2c_scan.cpp`)

We scanned twice — audio power (G45) **off**, then **on** (speaker amp G46 stays
off, so still silent). The audio chips should answer only when powered: if they
appear and disappear, the scan really detects presence (gotcha 29: an instrument
must be able to give a different answer).

### Bug — 16 "devices", including impossible ones

The first run listed `0x00–0x07` and `0x78–0x7C` — I²C **reserved** addresses no
chip uses — and the phantom list *differed* between the two scans.

```cpp
bool found[128];            // NOT initialised: leftover stack memory
M5.In_I2C.scanID(found);    // writes only entries 0x08..0x77
```

`scanID` skips 0–7 deliberately (its source comment: probing 0–7 hangs the S3's
I²C) and stops at 0x77, so 16 entries were never written — pure garbage.

> **C++ for Pythonistas:** `found = [False] * 128` is always initialised. A C++
> local array is **not** — it holds whatever was in that memory before. Fix:
> `bool found[128] = {};` (zero-fills). **Rule: initialise every local array.**
> The tell was that the garbage *changed* between runs; real devices don't.

### Result

```
audio power OFF:  0x32 RTC · 0x44 SHT40 · 0x6E PMIC                    → 3 devices
audio power ON:   + 0x18 ES8311 codec · 0x40 ES7210 mic ADC            → 5 devices
```

All five documented chips, at the documented addresses — and the scan provably
discriminates.

---

## Part 2 — An SHT40 driver by hand (`stages/lesson3_sht40.cpp`)

M5Unified has **no** SHT40 driver, so we wrote one (~40 lines) from the protocol:

1. Write **one byte**, `0xFD` — "measure T and RH, high precision".
2. Wait ~10 ms (datasheet maximum 8.2 ms).
3. Read **6 bytes**: `[T_hi T_lo CRC] [RH_hi RH_lo CRC]`.
4. Check each **CRC-8** (polynomial 0x31, initial value 0xFF).
5. Convert: `T = −45 + 175·raw/65535` °C; `RH = −6 + 125·raw/65535` %, clamped 0–100.

```cpp
bus.start(0x44, false, freq);  bus.write(0xFD);          bus.stop();  // "measure"
delay(10);
bus.start(0x44, true, freq);   bus.read(buf, 6, true);   bus.stop();  // collect
```

`read(..., true)` **NACKs the last byte** — the controller's "that's enough".

### Test the tester first

Before trusting `crc8()` to judge real data, it reproduced the datasheet's worked
example: `crc8(0xBE, 0xEF) = 0x92` → **PASS**. Then the chip's serial number,
CRC-checked: `F11A5C54 (CRC ok)`.

### Result

```
T  23.60 C   RH  53.4 %   raw 25690 / 31152   CRC ok   (9924 us)
T  23.59 C   RH  53.4 %   raw 25686 / 31136   CRC ok   (10217 us)
```

Hand-check: `0x64 0x5A` → `(0x64 << 8) | 0x5A` = 25690 → `−45 + 175 × 25690 / 65535`
= **23.60 °C** ✓.

> **C++ for Pythonistas:** `b[0] << 8 | b[1]` ≡ `int.from_bytes(b[0:2], "big")`.
> `Reading& out` is a **reference** — the function fills in the caller's own struct,
> like mutating a list you were passed. Functions return `bool` for failure; embedded
> C++ rarely uses exceptions.

### Precision vs accuracy

- **Resolution:** one count = 175/65535 ≈ **0.003 °C**; readings vary ±3 counts —
  very *precise*.
- **Accuracy:** datasheet typical ±0.2 °C / ±1.8 %RH — a separate question. And the
  sensor sits in a case beside a running ESP32 and PMIC, so it likely reads a little
  **warm** ("self-heating"), possibly creeping up after boot.

### `delay(10)` took 9.9 ms

The first read, *including* `delay(10)`, took **9,924 µs**. `delay()` counts FreeRTOS
1 ms ticks; starting mid-tick makes the first tick short, so `delay(10)` means
**9–10 ms**. Still above the sensor's 8.2 ms maximum — but `delay(9)` would not be
safe. **Rule: leave at least one tick of margin when waiting on hardware.**

---

## Part 3 — The real-time clock (`stages/lesson3_rtc.cpp`)

M5Unified drives the RX8130CE through `M5.Rtc`. Design decision: **the RTC stores
UTC; UK time (with BST) is computed in software** via a POSIX TZ rule
`GMT0BST,M3.5.0/1,M10.5.0`. M5Unified's own `setSystemTimeFromRtc()` already
assumes UTC, and the chip never needs to know about daylight saving.

### What it held: garbage, correctly rejected

```
raw 0x10..0x16 (sec min hour wday day month year): 50 11 07 29 01 00 C1
```

Registers are **BCD** — each 4-bit half is one decimal digit, so `0x50` means "50".
Month `00` and year `C1` are impossible: never set, or lost power. M5Unified
validates every field (its comment: guarding against "a bogus date such as 45:00:80
or year 2165"), so `getDateTime()` returned **false** — and we checked it.

### Setting it (`tools/rtc_sync.py`)

The host script waits for a **whole-second boundary**, then sends
`SET 2026-10-03 16:21:06` (UTC) over serial; the firmware parses it with `sscanf`
and calls `M5.Rtc.setDateTime()`. Afterwards:

```
raw: 06 21 16 40 03 10 26    → 16:21:06, weekday 0x40, 03 Oct 2026
```

The weekday register is **one-hot** — one bit per day; `0x40` = bit 6 = Saturday ✓
(computed with Sakamoto's day-of-week formula, no library).

### Bug — local time stuck on GMT (a real M5Unified bug)

`setSystemTimeFromRtc()` (`RTC_Class.inl:214–223`):

```cpp
char *oldtz = getenv("TZ");    // a POINTER into the environment
setenv("TZ", "GMT0", 1);       // overwrites that same entry
...
setenv("TZ", oldtz, 1);        // "restores" from the stale pointer → "GMT0"
                               // ...and never calls tzset()
```

> **C++ for Pythonistas:** a Python string can't change under you. A C `char*` is
> only an *address*; if something rewrites that memory, your "saved copy" changes
> too. That is a **dangling / stale pointer**.

Workaround without editing the library: re-apply our TZ and call `tzset()` after
every sync.

### Bug — calling it repeatedly (ours)

`setSystemTimeFromRtc()` also sets the sub-second part to **zero**. We first called
it every 10 s — each call could jerk the system clock by up to a second. Fix: sync
**once** (boot, and after SET); then let the ESP32's clock run.

### Bug — the two clocks a second apart: sync on the edge

After the fix, the RTC said `16:22:18` while the system clock said `…:17`. The RTC
reports **whole seconds**; copying "16:22:07" while the RTC was really at 16:22:07.9
starts the system clock up to 1 s late. Fix: wait for the RTC's seconds digit to
**tick over**, then copy:

```cpp
int s0 = M5.Rtc.getTime().seconds;
while (M5.Rtc.getTime().seconds == s0 && millis() - t0 < 1100) delay(1);
M5.Rtc.setSystemTimeFromRtc();
```

### Result

```
[tick] RTC UTC 2026-10-03 16:22:57 (Sat)  epoch 1791044577.852  local … 17:22:57 BST  [host-board = +0.01 s]
[tick] RTC UTC 2026-10-03 16:23:07 (Sat)  epoch 1791044587.853  local … 17:23:07 BST  [host-board = +0.00 s]
```

Board and host agree within ~10 ms; the RTC survived reflashes and resets; after
2 minutes unplugged it was still right (−0.02 s). *Caveat:* the board has a battery
and probably **kept running** while unplugged, so this tested "USB power lost", not
"board fully off" — that test belongs with deep sleep. The +0.01 → −0.02 s change
over ~8 min *might* be crystal drift, but USB timing jitter is ~10 ms: measuring
drift needs hours.

### Open question

The RX8130's flag register `0x1D` reads `0x27` both before and after setting the
time. M5Unified uses only bit 7 (battery-low, reads 0). The other bits' meanings
need the RX8130 datasheet — recorded, not guessed.

---

## Part 4 — Dashboard + the cold-refresh experiment *(next)*

---

## Files

| File | What |
|---|---|
| `stages/lesson3_i2c_scan.cpp` | internal bus scan, audio power off vs on |
| `stages/lesson3_sht40.cpp` | hand-written SHT40 driver with CRC self-test |
| `stages/lesson3_rtc.cpp` | RTC read/set, UTC↔BST, edge-synced system clock |
| `tools/rtc_sync.py` | set the RTC from the host's UTC; measure host–board offset |

## Glossary

- **I²C port** — one of the ESP32's I²C controllers; pins are routed to it.
- **ACK / NACK** — "byte received" / "stop sending"; the last read byte is NACKed.
- **CRC-8** — an 8-bit checksum that detects corrupted bytes.
- **Big-endian** — the high byte comes first.
- **Precision / accuracy** — how repeatable / how close to the truth.
- **BCD** — binary-coded decimal: each 4-bit half of a byte is one decimal digit.
- **One-hot** — exactly one bit set, its position meaning the value.
- **UTC / POSIX TZ rule** — universal time / a string describing a zone + DST rules.
- **Epoch** — seconds since 1970-01-01 00:00 UTC.
- **Stale (dangling) pointer** — an address whose contents changed or were freed.

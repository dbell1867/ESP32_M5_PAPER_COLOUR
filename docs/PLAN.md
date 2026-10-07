# Project Plan — M5Stack PaperColor (C++ learning)

A resumable roadmap. Each stage lists its goal and done-when; decisions and
open questions are recorded so a future session can pick up cold.

> **Resume here:** read `docs/lesson-01-papercolor-bringup.md` (bring-up: backup,
> serial, first light, buttons, microSD, LEDs) and
> `docs/lesson-02-inside-the-driver.md` (reading M5GFX's `Panel_ED2208`).
> Board facts: Lesson 01 "The hardware". Reusable workflow + gotchas: the
> `esp32-board-bringup` skill.
>
> **Current position (2026-10-07):** **Lesson 04 complete.** L1 standby v2 ran 24 h:
> 662 wakes, no gaps, 0 arm retries / 0 fallbacks / 0 power-off failures, −1.55 mV/h
> (~3 mA avg, ~2–3 weeks per charge, rough). v1's stall cause stays unproven (could be
> rare); v2's verified arming + fallback guard against it. **NEXT: choose Lesson 05**
> (ideas below).
>
> `src/main.cpp` currently holds the **L1 standby dashboard v2** (`stages/lesson4_standby_l1_v2.cpp`). Wake it with the POWER button (A/B/C don't work in L1).

---

## ✅ Done

- [x] **Setup** — user in `uucp`; PlatformIO 6.1.19; pioarduino **55.03.311**
      (55.03.312+ needs PlatformIO Core ≥ 6.2).
- [x] **Factory flash backup** — `backup/papercolor-factory-16MB.bin`, verified
      with `esptool verify-flash` (digest matched). Six blocks needed the `--no-stub`
      ROM reader. Restore: `esptool --chip esp32s3 --port /dev/ttyACM0 write-flash 0 backup/papercolor-factory-16MB.bin`
- [x] **Stage 1 — Hello serial** — chip, 16 MB flash, 8 MB PSRAM confirmed.
- [x] **Stage 2 — First light** — M5Unified/M5GFX (pinned 0.2.24 / 0.2.31); six
      exact inks, one refresh (16.16 s); orientation: (0,0) beside the two
      left-top buttons, 400 × 600 portrait.
- [x] **Stage 3a — Button probe** — A=G10, B=G9, C=G1 (top-centre); external
      pull-ups, active LOW, no rockers.
- [x] **Stage 3b — microSD** — 20 MHz via the **global `SPI`** M5GFX already owns;
      **G1 is not card-detect** (docs wrong; no card-detect line exists).
- [x] **Stage 3c — Buttons + LED** — LED preview / status, e-paper commit; skips
      refresh if already shown.
- [x] **Lesson 01** written.
- [x] **Skill updated** — phase 4 vendor-library question, phase 4b firmware
      backup, gotcha 25(b) "who owns the bus".
- [x] **Lesson 04** — deep sleep → power bench → 24 h tests → schematic → L1 standby
      v2 (verified timer arming): ~38 mA → ~3 mA average.
- [x] **Lesson 04 Parts 1–2** — deep-sleep dashboard; power bench (Grove 5 V = 24 mA;
      USB meter blind to the board's own load).
- [x] **Lesson 03** — I²C survey, SHT40 by hand (now `Sht40` class), RTC (UTC + BST),
      dashboard, fridge experiment (`docs/data/fridge-2026-10-03.csv`).
- [x] **Lesson 02** — driver reading, BUSY timeline, PSRAM check, palette +
      packing, dithering, simulator, and **Part 5: all predictions confirmed on
      the glass**; dithering costs ~0.6 s CPU (+567 ms fast, +606 ms text/quality).

## ▶ Next

- [ ] **Lesson 05 — pick one:** stretch battery life (refresh every 30 min / wake every
      5 min, measure); RTC-interrupt backup wake; Wi-Fi (NTP time sync, or show weather);
      a photo/image on the colour panel with your own dithering; the minimal own
      e-paper driver exercise.

## Later (candidates, not yet ordered)

- [ ] **RTC "board fully off" retention test** (PMIC power-off, then check the time).
- [x] **Skill fix (2026-10-04):** gotcha 19 corrected — wake modes differ by chip; check
      `soc_caps.h` (S3 has EXT0 + EXT1).
- [ ] **Battery / PMIC** — what M5PM1 (0x6E) can report.
- [ ] **Optional exercise** — a minimal 6-colour driver of our own, checked
      against `Panel_ED2208`; or app-side dithering of a photo region shown in
      `fastest` (crisp text + dithered image on one screen).
- [ ] **Untested:** how anti-aliased (smooth) fonts look in each mode.
- [x] **Skill restructure (2026-10-05)** — `SKILL.md` 9,079 → 1,779 words (workflow +
      principles + index); 13 topic files in `references/` hold G1–G54 (stable
      numbers; G42–G54 new from this project); `scripts/follow_serial.py`. Verified by a
      content-conservation check (606 tokens, 0 missing). Old version backed up in
      `~/Work/Micro/skill-backups/esp32-board-bringup-2026-10-05/`.

## Decisions (with reasons)

- **RTC holds UTC (2026-10-03):** UK time incl. BST computed via POSIX TZ; matches
  M5Unified's UTC assumption. Re-apply TZ after `setSystemTimeFromRtc()` (its stale
  `char*` bug) and sync on the RTC's second edge.
- **SHT40 via our own driver on `M5.In_I2C`** (M5Unified has none).

- **EPD mode by content (2026-10-03):** `fastest` for UIs in the six exact inks
  (crisp, 0.6 s faster); `quality` for photos (`fast` if warm tones matter).

- **Hybrid approach (2026-10-02):** build with M5Unified/M5GFX; learn the low
  level by reading `Panel_ED2208`. The key init bytes are vendor values we'd copy
  either way.
- **Silence:** G45/G46 driven LOW first thing; `cfg.internal_spk/mic = false`.
- **Lost presses during refresh — accepted (2026-10-02):** `display()` blocks
  ~16 s; the red status LED says "wait". Revisit with a core-0 task if an app
  needs input during refresh.
- **Pi boot card in the slot is read-only:** it belongs to a Raspberry Pi 4. Use a
  spare card for anything that writes.
- **Lesson 01 stays one document** (not split per stage).

## Open questions

- Why does the Grove 5 V boost draw 24 mA with nothing attached? (schematic)
- What power-path rule does the PMIC use (battery vs USB) while plugged in?

- RX8130 flag register `0x1D` reads `0x27` before and after setting — meaning of
  the bits other than bit 7 (battery-low)?
- How warm does the SHT40 read vs the room (self-heating)?

- Why does the booster command `0x06` use `…0x17` at init but `…0x27` before a
  refresh? (Unexplained in the driver.)
- What do the "CMDH" bytes (`0xAA 0x49 0x55 …`) and `0x84` do? (Vendor magic.)
- Does the controller switch waveform below ~8 °C? (Fridge test reached only 8.4 °C in
  the case; refresh was 49 ms *faster* cold. Don't use a freezer.)

# Project Plan — M5Stack PaperColor (C++ learning)

A resumable roadmap. Each stage lists its goal and done-when; decisions and
open questions are recorded so a future session can pick up cold.

> **Resume here:** read `docs/lesson-01-papercolor-bringup.md` (bring-up: backup,
> serial, first light, buttons, microSD, LEDs) and
> `docs/lesson-02-inside-the-driver.md` (reading M5GFX's `Panel_ED2208`).
> Board facts: Lesson 01 "The hardware". Reusable workflow + gotchas: the
> `esp32-board-bringup` skill.
>
> **Current position (2026-10-03):** Lesson 02 **Parts 1–4 done** — init table
> decoded, refresh measured phase-by-phase (BUSY-edge ISR: 14,413 ms panel refresh,
> 1,200 ms deliberate waits, 241 ms transfer), frame buffer measured (722,256 B
> PSRAM, 24-bit), nearest-ink matching (grey → green) and dithering incl. the
> pair-matching side effect (grey → teal), host simulator `tools/epd_sim.py`.
> **NEXT: Lesson 02 Part 5** — draw the simulator's test image on the glass in all
> four `epd_mode`s (A/B to step, one 16 s refresh each) and compare with
> `sim/compare.png`; write the results into Lesson 02.
>
> `src/main.cpp` currently holds the **PSRAM check** (`stages/lesson2_psram_check.cpp`).

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
- [x] **Lesson 02 Parts 1–4** — driver reading, BUSY timeline, PSRAM check,
      palette + packing, dithering, simulator. Written up (Part 5 pending).

## ▶ Next

- [ ] **Lesson 02 Part 5 — verify on the glass.** Port `pixel()` from
      `tools/epd_sim.py` to C++ (same integer maths); A/B cycle the four modes, C
      commits; LED 1 red while refreshing. *Done when:* the learner has compared
      each mode with `sim/compare.png` and the results (incl. whether greys look
      tinted) are written into Lesson 02.

## Later (candidates, not yet ordered)

- [ ] **Sensors** — SHT40 temperature/humidity (I²C 0x44); RX8130CE RTC (0x32).
      Includes the **cold-refresh experiment**: log temperature next to the BUSY
      REFRESH duration — does cold eat the 28 % timeout headroom?
- [ ] **Deep-sleep dashboard** — the job this board is built for: wake on RTC
      timer or a button (external pull-ups → no RTC-domain pull-ups needed), draw
      once, sleep. Measure real current (gotcha 20).
- [ ] **Battery / PMIC** — what M5PM1 (0x6E) can report.
- [ ] **Optional exercise** — a minimal 6-colour driver of our own, checked
      against `Panel_ED2208`.
- [ ] **Skill restructure** (separate job, needs approval) — workflow + principles
      in `SKILL.md`, topic-specific gotchas into reference files.

## Decisions (with reasons)

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

- Why does the booster command `0x06` use `…0x17` at init but `…0x27` before a
  refresh? (Unexplained in the driver.)
- What do the "CMDH" bytes (`0xAA 0x49 0x55 …`) and `0x84` do? (Vendor magic.)
- Does the real panel show greys as tinted as the simulator predicts? (Part 5.)
- Does cold slow the refresh toward the driver's 20 s timeout? (Sensors stage.)

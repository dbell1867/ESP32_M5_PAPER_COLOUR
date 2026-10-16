# Project Plan — M5Stack PaperColor (C++ learning)

A resumable roadmap. Each stage lists its goal and done-when; decisions and
open questions are recorded so a future session can pick up cold.

> **Resume here:** read `docs/lesson-01-papercolor-bringup.md` (bring-up: backup,
> serial, first light, buttons, microSD, LEDs) and
> `docs/lesson-02-inside-the-driver.md` (reading M5GFX's `Panel_ED2208`).
> Board facts: Lesson 01 "The hardware". Reusable workflow + gotchas: the
> `esp32-board-bringup` skill.
>
> **Current position (2026-10-11):** Lesson 08 complete — the house dashboard: hourly weather
> (Open-Meteo, HTTPS pinned to ISRG X1 + YR), new layout in `Screens.cpp`, battery %, `SHOT`
> screenshots, power-button pages (dashboard → Wi-Fi QR → flash photos). Lesson 06
> experiment commands removed. **Open:** post the M5Unified/M5GFX issue (owner's decision);
> next lesson not chosen yet.
>
> `src/main.cpp` currently holds the **house dashboard + pages + weather + photos + refresh log + light sleep + panel reset** (`stages/lesson8_nvs_fix.cpp`; 2026-10-14: NVS-full fix, packed logs — re-checked 2026-10-16: used 262 / available 242, on budget, settles ~312 used). Make sure `SIMV` is 0 (`tools/service.py LOG`). Wake it with the POWER button (A/B/C don't work in L1).

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
      **G1 is not card-detect** (docs wrong); the real one is M5PM1 GPIO1 (Lesson 07).
- [x] **Stage 3c — Buttons + LED** — LED preview / status, e-paper commit; skips
      refresh if already shown.
- [x] **Lesson 01** written.
- [x] **Skill updated** — phase 4 vendor-library question, phase 4b firmware
      backup, gotcha 25(b) "who owns the bus".
- [x] **Lesson 06** — photos: host-side error diffusion beats the driver on the glass;
      SD photos + photo-frame hold; refresh-time investigation; timeout patch script.
- [x] **Lesson 05** — Wi-Fi scan, runtime credentials (NVS `wifi`), SNTP sync with RTC
      drift measurement (~12.9 ppm), daily auto-sync. `docs/lesson-05-wifi-ntp.md`.
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

- [ ] Post (or not) `docs/m5gfx-issue-draft.md` to m5stack/M5GFX — owner's decision.
- [x] SD-card bus effect re-tested: none (Lesson 07). Photos now in flash (LittleFS).
- [x] **Lesson 08** — house dashboard: weather, pages, Wi-Fi QR. `docs/lesson-08-dashboard.md`.
- [ ] Lesson 09 ideas: a guest-network QR once the router supports it, photo upload over
      Wi-Fi, IR, the RGB LEDs, audio (user dislikes sounds); minimal own e-paper driver;
      RTC drift over days.
      (Battery: closed — enough for this board.)

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

- **Wi-Fi credentials (2026-10-07):** never in source/git/chat — `tools/wifi_setup.py`
  (getpass) → NVS namespace `wifi`; board reports only the password length.
- **NTP:** daily; skip in low battery; before drawing; Wi-Fi off after.

- **Battery protection (2026-10-07):** PMIC `BATT_LVP` 3.10 V backstop; firmware low
  3.50 V / empty 3.30 V, each confirmed on 2 consecutive wakes, ignored on USB.

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

- ~~Why 14–39 s refreshes?~~ Solved: no panel reset after power-up (Lesson 06 3g). Still unexplained: why it began on 7 Oct, not when L1 started.
- ~~What does an inserted SD card do to the shared SPI bus?~~ Nothing measurable (Lesson 07).

- Why does the Grove 5 V boost draw 24 mA with nothing attached? (schematic)
- What power-path rule does the PMIC use (battery vs USB) while plugged in?

- RX8130 flag register `0x1D` reads `0x27` before and after setting — meaning of
  the bits other than bit 7 (battery-low)?
- How warm does the SHT40 read vs the room (self-heating)?

- Why does the booster command `0x06` use `…0x17` at init but `…0x27` before a
  refresh? (Unexplained in the driver.)
- What do the "CMDH" bytes (`0xAA 0x49 0x55 …`) and `0x84` do? (Vendor magic.)
- ~~Does the controller switch waveform below ~8 °C?~~ Superseded: since 7 Oct it switches
  programs at ordinary temperatures too (Lesson 06 3c/3e).

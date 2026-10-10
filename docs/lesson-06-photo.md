# Lesson 06 — A Photo on a Six-Ink Panel

**Project:** M5Stack PaperColor (SKU C151) in C++ with PlatformIO
**Goal of this lesson:** Show real photos on the 400×600 Spectra 6 panel — and decide
*who* turns millions of colours into six inks: the display driver, or us.

> Audience note: Python / CircuitPython background, learning C++.
> **Status:** complete (Parts 1–4). One question is left for M5Stack: since 7 October the
> panel chooses among several refresh programs (14–39 s) at each power-up; before that it
> always took 14.4 s (Part 3).
>
> **Privacy:** the photos are personal. They, their previews and the converted panel
> images live in `images/`, which is **git-ignored**; only the tools and this text are
> committed. The photos are described here only in general terms.

---

## Learning objectives

1. Fit a phone photo to a panel: EXIF rotation, aspect ratio, crop, resize.
2. Understand **error diffusion** (Floyd–Steinberg) and why it suits photos.
3. Compare the driver's dithering with your own *before* touching the hardware.
4. Produce the exact bytes the panel takes (4 bits per pixel, 2 pixels per byte).
5. Investigate an intermittent hardware behaviour: log context, test hypotheses that can
   fail, and check new conclusions against old data.
6. Hook into a library without forking it (a weak function), and light-sleep while waiting.

---

## The two test photos

| | Content | Why it's a good test |
|---|---|---|
| Photo 1 | a ginger dog in a pastel collar with coloured pompoms, green sofa, confetti | warm browns (no orange ink), pastels (no pink / lilac / light-blue ink) |
| Photo 2 | a person in a dark coat beside a dark bronze statue, pale evening sky | lots of greys and dark neutrals — where Lesson 02 predicted the driver tints teal |

Both are 4032×3024-ish phone photos stored **sideways** with an EXIF orientation tag
(6 = "rotate 90° to view"); `magick -auto-orient` applies it. Upright they are **3:4**;
the panel is **2:3** (400×600), so the crop keeps the full height and trims a sliver
from each side (`--x/--y/--zoom` move or tighten it).

## Part 1 — Two routes, previewed on the computer (`tools/photo_prep.py`)

**Route A — let the driver dither.** Draw the full-colour photo; M5GFX converts at
`display()` time (`epd_quality` = pair matching with an arithmetic pattern; `epd_fast` =
Bayer). Previewed with Lesson 02's line-for-line port (`tools/epd_sim.py`).

**Route B — dither it ourselves**, on the computer, into the **six exact inks**, then show
it in `epd_fastest` (exact inks pass through untouched — Lesson 02).

### Floyd–Steinberg error diffusion

Go through the pixels in order; round each to the nearest ink; the **rounding error**
(what you wanted minus what you got) is not thrown away but **pushed onto the
neighbours you haven't visited yet**:

```
            [ x ]  7/16
     3/16    5/16  1/16
```

```python
r, g, b = buf[y][x]                       # includes error inherited from earlier pixels
pr, pg, pb, idx = nearest(r, g, b)        # nearest of the six inks
er, eg, eb = r - pr, g - pg, b - pb       # what we owe
for dx, dy, w in ((1,0,7), (-1,1,3), (0,1,5), (1,1,1)):
    buf[y+dy][x+dx] += (er, eg, eb) * w/16
```

Over any small area the inks therefore **average to the right colour** — a ginger patch
becomes a mix of red, yellow and black dots whose average *is* ginger, and a grey stays
grey because the error from a "too blue" dot is paid back by the next "too yellow" one.
*Ordered* dithering (Bayer, the driver's pattern) adds the same fixed nudge at a given
position regardless of the picture; error diffusion reacts to the picture itself.
**Serpentine** scanning (left-to-right, then right-to-left) avoids diagonal "worms".

### What the previews showed

| | Driver `quality` | Driver `fast` | **Ours (FS, serpentine)** |
|---|---|---|---|
| Photo 2 statue (dark bronze) | **green-teal** | **green-teal** | dark neutral bronze |
| Photo 2 sky (pale grey-blue) | pale, slightly tinted | **yellow-green** | pale, neutral |
| Photo 1 sofa / collar | greener sofa, bluer-purple collar | muddier | closer pinks/pastels, ginger fur kept |
| Texture | diagonal pattern | cross-hatch | fine, irregular grain |

Exactly the Lesson 02 prediction: the driver's pair matching turns greys and dark
neutrals blue-green. Error diffusion keeps them neutral.

Ink mix of our versions: Photo 1 — green 29 %, black 28 %, red 17 %, blue 14 %, yellow 6 %,
white 3 %; Photo 2 — green 27 %, blue 22 %, black 19 %, white 18 %, yellow 6 %, red 4 %.

**Caveat:** previews paint each ink with the driver's *assumed* RGB (`epd_palette[]`). The
real glass decides — especially how light "white" is and how saturated the inks are.

### The panel image

`pack()` writes exactly what the controller receives — 4-bit ink codes, left pixel in the
high nibble (Lesson 02): 400 × 600 / 2 = **120,000 bytes** per photo (`<name>.epd4`).

> **Python note:** the tool needs no imaging library — ImageMagick does the decode /
> rotate / crop / resize and hands back raw bytes (`rgb:-`), and Lesson 02's tiny PNG
> writer saves the previews.

---

## Part 2 — Photos on the real panel (`stages/lesson6_photos.cpp`)

### Storage: the SD card

A spare 64 GB card. `lsblk -f` (read-only) showed it was **already FAT32** (label
`M5SD`, 32 KB clusters) — no reformat needed; 64 GB cards usually ship exFAT, which the
ESP32 `SD` library can't mount (G25e/G39). Both versions of each photo go in `/photos`:
`<name>.epd4` (route B, ours) and `<name>.jpg` (the same 400×600 crop, route A). Copies
were checked byte-for-byte (`cmp`) and the card ejected with `udisksctl unmount` +
`power-off` so every write was flushed.

### Service commands added to the dashboard

| Command | Does |
|---|---|
| `PHOTOS` | list `/photos` |
| `SHOW <name>` | route B: unpack the 4-bit ink codes and draw each pixel in its **exact palette RGB**, refresh in `epd_fastest` (inks pass through untouched) |
| `SHOW <name> A` | route A: `M5.Display.drawJpgFile(SD, …)`, refresh in `epd_quality` (driver dithers) |

After `SHOW`, a **photo-frame hold** (NVS `hold`) stops scheduled dashboard refreshes —
the photo stays at zero power; wakes still log, sync and check the battery. The **power
button** returns to the dashboard; **low battery** always overrides the hold.

### Bug — `abstract type DataWrapperT<fs::SDFS>`: include order

M5GFX decides **at include time** whether to compile its file-reading support, by
checking which filesystem headers it has already seen. `#include <SD.h>` placed *after*
`<M5Unified.h>` → no `drawJpgFile(SD, …)`. Fix: include `SPI.h`/`SD.h` **first**.

> **C++ note:** headers are pasted in, top to bottom, at compile time. A library can test
> `#if defined(SOMETHING)` to see what came before it — so, unlike Python imports, the
> **order of `#include`s can change what a library contains**.

### On the glass

| | Driver (route A) | **Ours (route B)** |
|---|---|---|
| Photo 1 | "much more grainy, poor colour accuracy" | "close to the preview" |
| Photo 2 statue / dark coat | **greenish** | **neutral** |
| Photo 2 sky | **yellow tint** | **pale, neutral** |
| Grain | coarser, patterned | finer |

Every preview prediction held. Loading is fast (route B 258 ms, route A 225–238 ms);
the refresh is what takes time.

### UX lesson — the second button press

One press = dashboard refresh **then** photo refresh ≈ 45–60 s with no obvious feedback,
so a second press felt natural — and the power button *ends* photo mode. The service
window now **blinks LED 0 blue** the whole time it is open ("awake and listening").

### Tool bugs (mine)

| Symptom | Cause | Fix |
|---|---|---|
| scan results never arrived | tool stopped after 2 s silence; a scan *is* seconds of silence | longer quiet timeout (now 40 s) |
| photo refreshed **twice** | `service.py` sent commands on connect **and** again on the window line | send once per connection |
| tool "succeeded" without doing anything | `termios.error` (port vanishing mid-open) is neither `SerialException` nor `OSError` → crash; my `grep` hid the traceback **and** the exit code | catch any exception and keep waiting |

## Part 3 — The refresh time that changed (an investigation)

### 3a. It doubled (7 Oct)

Every photo refresh took ~28 s. Lesson 02 had measured ~16 s. Lesson 02's BUSY-pin timer
(now built into every refresh: `timedDisplay()`) was used to bisect:

| Test | Transfer | Panel phases (on / REFRESH / off) | Total |
|---|---|---|---|
| Lesson 02, 3 Oct | 241 ms | 131 / **14,413** / 152 | 16,141 |
| scheduled refreshes until 13:52 today | — | — | wakes of 16–17 s (normal) |
| dashboard, **card in** (after 13:56) | **639** | one merged LOW of 26,670 | 27,713 |
| dashboard, card **out** | 241 | 131 / **26,695** / *(missing)* | 28,074 |
| dashboard after dashboard | 241 | 131 / 26,691 / *(missing)* | 28,065 |
| **original Lesson 02 program** | 639 | merged 26,672 | 27,713 |
| … with the driver timeout raised to 60 s | 241 | 131 / **26,694** / **152** | 28,426 |
| all **white** / all **black** / six ink bands | 241 | REFRESH 26,689 / 26,689 / 26,691 | 28,417 each |

What it establishes:

1. **The panel's own refresh doubled — 14.4 s → 26.7 s — from about 13:56 on 7 October**,
   and stayed slow for hours: across firmware (even the original Lesson 02 program), with
   or without the card, regardless of the previous image, and after a complete refresh.
   (Later the same day it came and went — 3b.)
2. **Content doesn't matter:** white, black and six ink bands are identical to ±2 ms.
   This panel runs one fixed sequence per refresh (and colour count was not what
   changed — Lesson 02's six-ink test image took 14.4 s too).
3. **The driver's 20 s timeout was now cutting refreshes short.** `_wait_busy()` gives up at
   20 s and its caller ignores the result, so POWER_OFF was sent **mid-refresh** — visible
   as the missing power-off phase. Lesson 02 flagged this risk ("28 % headroom"); it
   became real. With 60 s, the three phases come back cleanly.
4. **An inserted SD card disturbs the shared bus:** transfer 241 → 639 ms, and BUSY
   edges glitch/merge. A separate, smaller effect.
5. **Not our code:** the factory firmware uses M5GFX **0.2.21** (commit `5268353`, 15 May)
   whose `Panel_ED2208` init bytes, refresh sequence and 20 s timeout are **identical**.

At this point, ruled out by measurement: our firmware, previous content, the card at
refresh time, colour content, an interrupted-refresh loop. M5Stack quotes "15–30 s"
(and elsewhere "10–20 s"), so 26.7 s is inside their range.

> **Method note (gotcha 22):** after three plausible hypotheses failed, the useful moves
> were controlled comparisons — same program, same screen, then vs now; card in vs out;
> colour vs none — each designed so the result *could* come out differently.

### 3b. It comes and goes (7 Oct, afternoon)

| Time | REFRESH | Situation |
|---|---|---|
| until 13:52 | ~14.4 s | dashboard, scheduled refreshes |
| 13:56 – 15:32 | ~26.7 s | photo firmware, test programs, restored dashboard |
| ~16:18 | "15–16 s" by stopwatch | **factory firmware** (flashed from the backup; NVS saved and restored around it) |
| 16:24 | 14.5 s | our dashboard, first boot straight after the factory firmware |
| 16:31 | 14.2 s ×3 | first boot after an upload, then two REDRAWs |
| 16:38 | 26.7 s ×2 | first boot after an upload, then a REDRAW |

- **The factory firmware "fixing" it was coincidence.** It looked decisive — slow for
  hours, factory firmware, fast again — but minutes later our own unchanged firmware was
  slow again. *With an intermittent fault, one before/after is not evidence.*
- **Falsified:** the factory firmware drives PM1 GPIO4 (`SD_DET_EN`) HIGH; ours leaves it an
  input. The panel was fast with GPIO4 still an input, and setting it HIGH (`SDDET 1`)
  changed nothing. Power-chip registers in the slow state:
  `docs/data/pmic-regs-slow-2026-10-07.txt` (`PMDUMP`).
- Side finding: the board's real **card-detect** is **PM1 GPIO1** (`SD_DEC`, read through the
  power chip) — which is why ESP32 G1 never saw the card.

Six hypotheses had failed. **Gotcha 22: stop guessing, collect data.**

### 3c. A refresh log (`RLOG`)

Every refresh now stores a 16-byte record in NVS: time, REFRESH and transfer ms, battery
mV, temperature, minutes since the previous refresh, PMIC wake source, USB present, kind
(dashboard/photo/redraw) and — later — whether the ESP32 light-slept. Records are grouped
16 per NVS blob (one key per record would cost ~32 bytes of overhead each and fill the
20 KB partition); 12 blobs hold the last ~180 refreshes. `RLOG` dumps it as CSV.

First 17 hours (`docs/data/refresh-log-2026-10-08.csv`, 113 refreshes):

| REFRESH | Count | Board temp (SHT40) |
|---|---|---|
| **14.2 s** | 26 | 19.9–22.8 °C only |
| **25.0 s** | 52 | 14–20 °C |
| **26.6–26.8 s** | 26 | 9.8–13 °C, *and* 19–23 °C |
| 33–39 s | 8 | scattered |

- **Discrete levels, not drift:** the panel runs one of a few fixed programs.
- **USB doesn't matter:** fast and slow both occur on USB (21 fast / 8 slow) and on battery (6 / 78).
- **Temperature does matter now:** in a cold spell the board fell to 9.8 °C and back, and
  the time stepped with it — 14.2 → 25.0 → 26.6 s (≤13 °C) → 25.0 → 14.2 s.
- **Within one wake it never changes.** Every REDRAW in a wake matched that wake's first
  refresh (3 of 3, 2 of 2, later 10 of 10). It only changes *between* wakes — and every
  standby powers the panel controller fully off.

### 3d. Telling the controller the temperature (no effect)

Controllers in this family (the ED2208's init bytes match the UC81xx command set) accept a
forced temperature: `CCSET 0xE0 = 0x02` ("use TSSET"), then `TSSET 0xE5 = °C`. M5GFX never
sends these. The panel's chip-select is G44, and `M5.Display.getPanel()->getBus()` gives
the bus, so a sketch can send them itself.

| Test | When the temperature was sent | Result |
|---|---|---|
| `TSWEEP` | in one wake, before each of 9 redraws: internal, 25, 21, 18, 12, 5, 30, 40 °C, internal | **35.3 s every time** (±5 ms) |
| `TFSWEEP` | before the **first** refresh after a hardware reset of the controller (RST G43), one restart per step: internal, 25, 12, 40, 0 °C, internal | **26.7 s every time** (±5 ms) |

The ED2208 **ignores** these commands (or uses them for something else). Six hardware
resets in a row also kept the same program — so the choice is made at **power-up**, not at
reset, and we can't steer it from outside.

### 3e. Putting it together — and a correction

The tempting conclusion after 3c was *"the panel picks its program by temperature; that's
normal for e-paper"* (cold pigment moves slowly, so colder bands get longer drive
sequences). **Checking it against older data killed it.** Lesson 03's fridge test
(3 October) ran from 23.5 °C down to **8.4 °C** and every refresh took **14.4 s**. Since
7 October the same temperatures give 14, 25, 27 or 35 s.

What the evidence supports:

1. **Before 7 Oct ~13:56:** one program, ~14.4 s, at every temperature tested (8–24 °C).
2. **Since then:** a choice among several programs, made at each **power-up** and fixed until
   the next one; temperature shifts the choice, but the same temperature can give
   different programs.
3. **Something about the panel changed at 13:56 on 7 October**, and it persists across
   firmware, resets and power cycles.

What it might be — **speculation, not evidence:** the controller may keep state in its own
non-volatile memory (a refresh/age counter that switches to stronger "conditioning"
programs, or a setting altered by garbled bus traffic — 13:56 was the first boot with the
SD card disturbing the shared bus). Only the ED2208 datasheet or M5Stack can say; it's
now the central question in the issue draft.

**What it means in practice:** refreshes take **14–39 s**, so the driver's 20 s timeout is
too short for most of them; the 60 s patch (Part 4) covers the whole range. *Never size a
timeout from one measurement.*

### 3f. Light sleep during the refresh

The slow programs cost battery. During a refresh the panel does the work and the ESP32
only waits for BUSY, so it can **light-sleep** instead:

- `tools/patch_m5gfx.py` now also replaces the `delay(10)` in the driver's wait loop with a
  call to `m5gfx_ed2208_busy_wait_hook()` — a **weak** function whose default is that same
  `delay(10)`, so the library behaves exactly as before unless the firmware defines its own.
- Ours sleeps until BUSY goes HIGH, or 1 s at most (so the driver's 60 s timeout still
  counts), with the timing ISR paused and the missed rising edge recorded on waking.
- Checked with `nm` that the linker took **ours** (186 bytes, from `main.cpp`), not the
  two-line default — a weak symbol that loses fails silently.
- Every refresh still completes normally, and the **USB serial port survived** light sleep
  on this ESP32-S3. `LS 0/1/?` switches it (default: on battery only; currently forced on).

| | 8–9 Oct, no light sleep | **9–10 Oct, light sleep** |
|---|---|---|
| On battery | 17.2 h, 99 refreshes | 21.3 h, 124 refreshes |
| Average refresh | 24.9 s | 21.3 s |
| Drain, same band 4.108–4.160 V | −3.68 ± 0.10 mV/h | **−3.18 ± 0.17 mV/h** |

About **14 % less** — but the refreshes were also 13 % shorter that day (1 °C warmer), so
the light-sleep share **can't be separated**: "between a little and 14 %". That's far less
than expected if the waiting ESP32 drew ~40 mA, so the **panel's own drive** is most likely
the main cost of a refresh. Kept, because it's free. The real lever would be refreshing less
often — deliberately not done (enough battery work for this board).

### Bugs of mine along the way

| Symptom | Cause | Fix |
|---|---|---|
| date drawn 3× too large and fuzzy after the sweep | sweep screens `setTextSize(3)`; the dashboard's date line inherited it (a bitmap font scaled 3× looks blocky) | reset after the sweep; every screen now starts with `setTextSize(1)` |
| upload failed: "Unable to verify flash chip connection"; a garbled serial line | my earlier 8-minute `follow_serial.py` was still reading the port (G66) | wait for or stop helpers before uploading |
| a wait loop that never ended | `pgrep -f 'follow_serial…'` matched its **own** command line (G66) | stop tasks by ID |
| "198 refreshes" in a day | the refresh log arrived in two service windows and I saved both copies | de-duplicate by record number: 99; voltage fits unchanged |
| two all-zero rows in `RLOG` | once the ring wraps, the blob being filled has overwritten the oldest | dump only the valid 11 blobs + the current partial one |

## Part 4 — What was changed because of it

- **`tools/patch_m5gfx.py`** — a PlatformIO `extra_scripts = pre:` script that re-applies two
  changes to the downloaded M5GFX before every build (the library lives in git-ignored
  `.pio/libdeps/`): the busy timeout **20 s → 60 s**, and the **busy-wait hook** (3f). Both
  exact-match the original text, are idempotent, and warn loudly if M5GFX changes.
- **Light sleep** while the panel refreshes (3f).
- **Every refresh is timed and logged** (`refresh timing [...]` on serial, `RLOG` in NVS,
  `refresh_ms` in the hourly battery log).
- Service commands for the investigation: `PMDUMP`, `REDRAW`, `SDDET 0/1`, `TSWEEP`,
  `TFSWEEP`/`TFRES`, `LS 0/1/?`.
- **SD card kept out of the board** until the bus disturbance is understood.
- **Open:** ask M5Stack (draft: `docs/m5gfx-issue-draft.md`) what changed on 7 October and
  how the controller chooses its program.

## Files

| File | What |
|---|---|
| `tools/photo_prep.py` | crop + resize, driver-mode previews, Floyd–Steinberg, packed `.epd4` + 400×600 `.jpg` |
| `stages/lesson6_photos.cpp` | dashboard + `PHOTOS`/`SHOW`, photo hold, refresh timing, blinking service LED |
| `stages/lesson6_colour_refresh_test.cpp` | white / black / six-band refresh timing test |
| `stages/lesson6_sddet_test.cpp` | + `PMDUMP`, `SDDET`, `REDRAW` (GPIO4 test) |
| `stages/lesson6_refresh_log.cpp` | + per-refresh log (`RLOG`) |
| `stages/lesson6_tsweep.cpp` / `lesson6_tfsweep.cpp` | forced-temperature tests, in-wake and per reset |
| `stages/lesson6_light_sleep.cpp` | + light sleep during refresh (= current `src/main.cpp`) |
| `tools/patch_m5gfx.py` | pre-build: M5GFX ED2208 busy timeout 20 s → 60 s, busy-wait hook |
| `docs/data/colour-refresh-test-2026-10-07.log` | the colour-content timing run |
| `docs/data/pmic-regs-slow-2026-10-07.txt` | power-chip registers in the slow state |
| `docs/data/refresh-log-2026-10-08/09/10.csv` | per-refresh logs (3c, 3f) |
| `docs/data/battery-2026-10-09/10.csv` | hourly battery logs of the two battery days (3f) |
| `images/<name>/` (git-ignored) | `source.png`, `driver_*.png`, `fs*.png`, `compare.png`, `<name>.epd4` |

```bash
python3 tools/photo_prep.py images/<photo>.jpg [--x 0.5 --y 0.5 --zoom 1.0]
xdg-open images/<photo>/compare.png    # source | driver quality | driver fast | ours
```

## Glossary

- **EXIF orientation** — a tag saying how to rotate a photo for viewing.
- **Aspect ratio** — width : height (photo 3:4, panel 2:3).
- **Error diffusion / Floyd–Steinberg** — dithering that passes each pixel's rounding error
  on to its neighbours.
- **Ordered dithering** — a fixed position-based threshold pattern (e.g. Bayer).
- **Serpentine scan** — alternate row direction while diffusing.
- **Waveform / refresh program** — the sequence of voltage pulses that moves the pigments;
  stored in the panel, chosen by the controller.
- **Light sleep** — the CPU pauses with RAM kept; it wakes on a pin or timer and carries on
  (unlike deep sleep, which reboots).
- **Weak symbol** — a function definition the linker uses only if no other ("strong")
  definition exists.

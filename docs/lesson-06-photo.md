# Lesson 06 — A Photo on a Six-Ink Panel

**Project:** M5Stack PaperColor (SKU C151) in C++ with PlatformIO
**Goal of this lesson:** Show real photos on the 400×600 Spectra 6 panel — and decide
*who* turns millions of colours into six inks: the display driver, or us.

> Audience note: Python / CircuitPython background, learning C++.
> **Status:** complete (Parts 1–4), with one **open question**: why this panel's refresh
> doubled from 14.4 s to 26.7 s on 7 October (Part 3).
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

## Part 3 — The refresh that doubled (open question)

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

1. **The panel's own refresh doubled — 14.4 s → 26.7 s — at about 13:56 on 7 October**,
   and it persists: across firmware (even the original Lesson 02 program), with or
   without the card, regardless of the previous image, and after a complete refresh.
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

Not established: **why** the panel changed. Ruled out by measurement: our firmware,
previous content, the card at refresh time, colour content, an interrupted-refresh loop.
Remaining ideas need the controller's datasheet: its internal temperature reading (but the
fridge test showed no step between 8 and 24 °C), or a persistent controller setting
changed by garbled bus traffic while the card disturbed the bus (13:56 was the first boot
with the card in under the new firmware) — speculation, not evidence. M5Stack quotes
"15–30 s" (and elsewhere "10–20 s"), so 26.7 s is inside their range.

> **Method note (gotcha 22):** after three plausible hypotheses failed, the useful moves
> were controlled comparisons — same program, same screen, then vs now; card in vs out;
> colour vs none — each designed so the result *could* come out differently.

## Part 4 — What was changed because of it

- **`tools/patch_m5gfx.py`** — a PlatformIO `extra_scripts = pre:` script that re-applies the
  20 s → 60 s timeout fix to the downloaded M5GFX before every build (the library lives
  in git-ignored `.pio/libdeps/`). Verified: it re-patched after a deliberate revert, and
  is idempotent. Every refresh now ends with a proper 152 ms power-off phase.
- **SD card kept out of the board** until the bus disturbance is understood.
- **Every refresh is timed** (`refresh timing [...]` on serial) — if the panel ever changes
  back, the log will show when.
- **Open:** ask M5Stack (draft: `docs/m5gfx-issue-draft.md`); optionally time a refresh
  under the factory firmware.

## Files

| File | What |
|---|---|
| `tools/photo_prep.py` | crop + resize, driver-mode previews, Floyd–Steinberg, packed `.epd4` + 400×600 `.jpg` |
| `stages/lesson6_photos.cpp` | dashboard + `PHOTOS`/`SHOW`, photo hold, refresh timing, blinking service LED |
| `stages/lesson6_colour_refresh_test.cpp` | white / black / six-band refresh timing test |
| `tools/patch_m5gfx.py` | pre-build fix: M5GFX ED2208 busy timeout 20 s → 60 s |
| `docs/data/colour-refresh-test-2026-10-07.log` | the colour-content timing run |
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

# Lesson 06 — A Photo on a Six-Ink Panel

**Project:** M5Stack PaperColor (SKU C151) in C++ with PlatformIO
**Goal of this lesson:** Show real photos on the 400×600 Spectra 6 panel — and decide
*who* turns millions of colours into six inks: the display driver, or us.

> Audience note: Python / CircuitPython background, learning C++.
> **Status:** Part 1 (on the computer) done. On-device display not started — the board
> is mid-way through measuring RTC drift (Lesson 05).
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

## Files

| File | What |
|---|---|
| `tools/photo_prep.py` | crop + resize, driver-mode previews, Floyd–Steinberg, packed `.epd4` |
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

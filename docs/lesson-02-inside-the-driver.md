# Lesson 02 — Inside the Driver: How M5GFX Talks to a Spectra 6 Panel

**Project:** M5Stack PaperColor (SKU C151) in C++ with PlatformIO
**Goal of this lesson:** Get the low-level understanding we skipped in Lesson 01
— by *reading* the working driver (`Panel_ED2208`) rather than rewriting it —
and check every claim we make about it against a **measurement**.

> Audience note: written for a Python / CircuitPython programmer learning C++.
> This is the "hybrid" half of Lesson 01's decision: **build** with
> M5Unified/M5GFX, **learn** by reading the driver. "Gotcha N" refers to the
> `esp32-board-bringup` skill's playbook.

> **Status:** complete (Parts 1–5).

---

## Learning objectives

By the end of this lesson you can:

1. Trace a call from `M5.Display.display()` to ink moving on the panel.
2. Decode a vendor init table, and separate what is **known**, **likely** and
   **unknown** in it.
3. Explain the BUSY handshake, and account for every millisecond of a 16 s refresh
   with a measurement.
4. Explain why the frame buffer is 720 KB when the panel receives 120 KB — and
   what the extra buys.
5. Explain nearest-colour matching, and why mid-grey becomes **green**.
6. Explain ordered dithering (Bayer and an arithmetic pattern) and the driver's
   **pair-matching** trick, including its side effect on greys.
7. Use a host-side **simulator** to predict a result before spending hardware time.

---

## Where the code is

Read the copy you actually **compile against**, not the newest one on GitHub:

```
.pio/libdeps/papercolor/M5GFX/            (M5GFX 0.2.31, pinned in platformio.ini)
  src/lgfx/v1/panel/Panel_ED2208.hpp       79 lines — class + init bytes
  src/lgfx/v1/panel/Panel_ED2208.inl      487 lines — everything else
```

`.inl` is just C++ source that another file `#include`s (the library compiles
everything through one `lgfx_v1.cpp`); treat it like a `.cpp`.

---

## Part 1 — The map and the init bytes

### The call path

```
your drawString / fillRect ──► write RGB pixels into the 720 KB frame buffer (PSRAM)
                               and widen a "dirty rectangle" (_range_mod)
M5.Display.display()
  └─ Panel_ED2208::display()              .inl:400
       ├─ nothing drawn? → return          (no drawing = no refresh)
       ├─ _exec_transfer()                 .inl:428  convert rows to inks, send over SPI
       └─ _turn_on_display()               .inl:319  power on → refresh → power off
```

**Discovery:** the driver tracks a dirty rectangle but then sends the **whole
screen** anyway — Spectra 6 has no partial refresh. The rectangle is only an
"anything changed?" flag. **One changed pixel costs the full 16 s.**

### The init table (`.hpp:51–70`)

```cpp
static constexpr uint8_t list0[] = {
  0xAA,  6, 0x49, 0x55, 0x20, 0x08, 0x09, 0x18,  // CMDH
  0x01,  1, 0x3F,
  0x00,  2, 0x5F, 0x69,
  ...
  0xFF, 0xFF, // end
};
```

It's **data, not code**: `[command] [count] [count data bytes]…` until `0xFF 0xFF`,
walked by `Panel_Device::command_list()` (`Panel_Device.inl:193`). In Python:

```python
INIT = [(0xAA, [0x49, 0x55, 0x20, 0x08, 0x09, 0x18]), (0x01, [0x3F]), ...]
for cmd, data in INIT:
    send_command(cmd)
    for b in data:
        send_data(b)
```

A `constexpr` byte array costs nothing at runtime and lives in flash, not RAM.

| Command | Meaning | Confidence |
|---|---|---|
| `0x61` + 400, 600 | Resolution — **computed** in `_init_sequence` (`.inl:308`): 400 = `0x01 0x90`, 600 = `0x02 0x58`, high byte first | Certain |
| `0x04` / `0x12` / `0x02` / `0x10` / `0x07 0xA5` | Power on / refresh / power off / start pixel data / deep sleep (`0xA5` = "really sleep" check code) | Certain (named in the driver's comments) |
| `0x00`, `0x01`, `0x06`, `0x50`, `0xE3` | Panel settings, power settings, booster, border/VCOM, power saving — the conventional meanings in this controller family | Likely, not confirmed by a datasheet |
| `0xAA 0x49 0x55 …` ("CMDH"), `0x84`, the exact voltage values | Panel-maker tuning for this particular glass | **Unknown — copy, don't change** |

That bottom row is *why* we didn't go low-level: the most important bytes can be
copied but not derived.

**A puzzle left honestly open:** the booster command `0x06` is sent with
`0x6F 0x1F 0x17 0x17` at init but `0x6F 0x1F 0x17 0x27` just before every refresh.
The code doesn't say why. Plausibly a stronger drive for the refresh — but that is
a guess, and labelled as one.

---

## Part 2 — One refresh, step by step

### Read the code

`_turn_on_display()` (`.inl:319`):

```cpp
_send_command(0x04);  _wait_busy();  delay(200);          // POWER ON
_send_command(0x06);  /* 4 bytes */  delay(200);          // booster (…0x27)
_send_command(0x12);  _send_data(0x00);  _wait_busy();    // REFRESH: particles move
_send_command(0x02);  _send_data(0x00);  _wait_busy();  delay(200);  // POWER OFF
```

`_wait_busy()` (`.inl:278`):

```cpp
if (!gpio_in(BUSY)) {                      // BUSY LOW = "I'm working"
  do { if (millis() - start > timeout) return false;  delay(10); }
  while (!gpio_in(BUSY));                   // poll every 10 ms
  delay(200);                               // hidden settle time
}
return true;
```

Notes from reading:

- **Power is off between refreshes** — the panel's high voltages exist only during
  the refresh. If you ever write your own driver, this is the line not to forget.
- **An ignored return value.** `_wait_busy` gives up after **20 s** and returns
  `false`; `_turn_on_display` ignores it. E-paper generally refreshes more slowly
  when cold, so in a cold room the driver could, *in principle*, send POWER OFF
  mid-refresh. A hypothesis, not a confirmed bug (and the gotcha 29 pattern:
  unchecked return values).

**Prediction from the code:** 3 explicit `delay(200)` = 0.6 s, plus 0–3 hidden
200 ms waits, plus ~0.24 s to send 120,000 bytes at 4 MHz → roughly **1.0–1.5 s of
ESP32-side time**; the rest is the panel. (Learner's guess before measuring: "about
1 second".)

### Measure it — the BUSY timeline (`stages/lesson2_busy_timeline.cpp`)

An interrupt **listens** to the BUSY pin (G11) and timestamps every edge while
M5GFX's `display()` runs; the driver still owns the pin.

```cpp
static void IRAM_ATTR onBusyEdge() {
  int n = nEdges;
  if (n < MAX_EDGES) {
    edgeUs[n]    = micros();
    edgeLevel[n] = (REG_READ(GPIO_IN_REG) >> PIN_EPD_BUSY) & 1;  // ISR-safe pin read
    nEdges = n + 1;
  }
}
attachInterrupt(digitalPinToInterrupt(PIN_EPD_BUSY), onBusyEdge, CHANGE);
```

> **C++ / embedded notes.** `IRAM_ATTR` places the function in internal RAM so it
> can run even while flash is busy. `volatile` tells the compiler the value can
> change behind its back (the ISR writes it, `loop()` reads it) — so it must
> really re-read it every time. The ISR does the bare minimum: no `Serial`, no
> allocation. Reading `GPIO_IN_REG` directly avoids calling `digitalRead`, which
> isn't guaranteed to be safe inside an interrupt.

Each refresh also **prints the previous measurement on the screen** — the panel
displays the timing of its own last refresh.

### Result — two runs, identical to the millisecond

```
transfer   :    241 ms before 1st BUSY
POWER_ON   :    131 ms busy (at 241)
REFRESH    :  14413 ms busy (at 973)
POWER_OFF  :    152 ms busy (at 15590)
edges: 6   total: 16141 ms
```

| ms | What | Source |
|---|---|---|
| 0 → 241 | 120,000 bytes over SPI (prediction 0.24 s ✓) | `_exec_transfer` |
| 241 → 372 | Panel: POWER_ON (131 ms) | |
| 372 → 973 | ESP32 waits: 200 hidden + 200 + 200 = 600 ms ✓ | `_wait_busy`, `delay(200)` ×2 |
| 973 → 15,386 | Panel: REFRESH (14,413 ms) | |
| 15,386 → 15,590 | ESP32 waits: 200 hidden ✓ | `_wait_busy` |
| 15,590 → 15,742 | Panel: POWER_OFF (152 ms) | |
| 15,742 → 16,141 | ESP32 waits: 200 hidden + 200 ✓ | |

| Where the time goes | ms | share |
|---|---|---|
| Panel physically working | 14,696 | 91 % |
| Deliberate ESP32 waits (6 × 200) | 1,200 | 7.4 % |
| SPI transfer | 241 | 1.5 % |

Conclusions:

1. **The model held.** Every gap matched a specific line of code to within a few
   ms — and the measurement *could* have contradicted it.
2. **Optimising the driver is nearly pointless.** Deleting every software wait
   would save ~1.4 s of 16; 91 % is fixed by the panel. The 200 ms waits are likely
   vendor settle times — not worth the panel risk.
3. **The panel's timing is deterministic** (identical across runs, at this
   temperature).
4. **Timeout headroom is ~28 %** (14.4 s of 20 s). Whether cold eats it is now a
   testable question — a natural experiment once the SHT40 temperature sensor is
   working.

(Lesson 01 measured 16,160 ms with `millis()` around `display()` +
`waitDisplay()`; 16,141 ms here comes from `micros()` at slightly different
points. The panel didn't change; the stopwatch placement did.)

---

## Part 3 — From your RGB to inks

### The ink codes (`.inl:33–40`)

```cpp
EPD_BLACK = 0x0, EPD_WHITE = 0x1, EPD_YELLOW = 0x2, EPD_RED = 0x3,
EPD_BLUE  = 0x5, EPD_GREEN = 0x6     // "0x4 and 0x7 are unused"
```

4 bits per pixel. Codes 4 and 7 are "unused" with no reason given; most likely
reserved by the controller for colours this panel lacks — a guess.

### Choosing an ink (`_rgb_to_epd_color`, `.inl:76`)

```python
best = min(PALETTE, key=lambda p: (r-p.r)**2 + (g-p.g)**2 + (b-p.b)**2)
```

The nearest point in the RGB cube (no square root — it doesn't change which point
is nearest). The palette holds roughly what each ink **looks like**, not ideal
screen colours: yellow (255, 243, 56), blue (100, 64, 255), green (67, 138, 28).

### Prediction: what common colours become (no dithering)

Computed on the host with the driver's exact function:

| Colour | RGB | Becomes |
|---|---|---|
| `TFT_ORANGE` | 255, 182, 0 | YELLOW |
| `TFT_CYAN` | 0, 255, 255 | BLUE |
| `TFT_MAGENTA` | 255, 0, 255 | BLUE |
| `TFT_PURPLE` | 123, 0, 123 | RED (BLUE a close second) |
| `TFT_NAVY` | 0, 0, 123 | BLACK |
| `TFT_PINK` | 255, 195, 206 | WHITE |
| **`TFT_DARKGREY`** | 123, 125, 123 | **GREEN** |

A grey ramp: **0–52 BLACK · 53–147 GREEN · 148–167 BLUE · 168–255 WHITE**.

*Why:* "nearest in the RGB cube" isn't "nearest to the eye". The green ink is a
dark, fairly neutral point, so it is closer to mid-grey than black or white is.
**Rule for `epd_fastest`: design with the six inks only.** A "greyed-out" button
would be green.

### Packing two pixels per byte (`_dither_row_none`, `.inl:146`)

```cpp
dst[x >> 1] = (c0 << 4) | c1;
```

| C++ | Python | Meaning |
|---|---|---|
| `x >> 1` | `x // 2` | two pixels share one byte |
| `c0 << 4` | `c0 * 16` | left pixel in the **high** nibble |
| `\| c1` | `+ c1` | right pixel in the **low** nibble |

RED then BLUE → `0x35`. A 400-pixel row → 200 bytes; 600 rows → **120,000 bytes**
(the 241 ms transfer above).

### The frame buffer: 720 KB — and why "6×" isn't waste

The frame buffer is allocated once in `Panel_ED2208::init` (`.inl:349`):

```cpp
setColorDepth(color_depth_t::rgb888_3Byte);                 // .inl:353
uint32_t bytes_per_line = pw * sizeof(bgr888_t);            // 400 × 3 = 1,200
_framebuffer = heap_alloc_psram(bytes_per_line * ph);        // × 600 = 720,000
```

| How a pixel could be stored | Bytes/pixel | Whole screen |
|---|---|---|
| **RGB888 — what the driver keeps** | 3 | **720,000** |
| RGB565 — typical for LCDs | 2 | 480,000 |
| **4-bit ink code — what the panel receives** | 0.5 | **120,000** |

720,000 ÷ 120,000 = **6×**. The 4-bit image is **never stored**: `_exec_transfer`
converts **one row at a time** into a reused 200-byte buffer. And the choice is
**enforced** — `setColorDepth()` ignores whatever you ask for and always returns
24-bit (`.inl:260–266`).

*Correction made during the lesson:* I first called this "6× more memory than
strictly necessary". That's wrong. A 4-bit buffer would force the ink decision at
**draw time**, losing the original colours, which breaks: (1) pair dithering, which
needs both neighbours' true colours; (2) anti-aliased text and blending, which
read the existing pixel; (3) changing `epd_mode` without redrawing. The honest
statement: **6× the most compact form, and the extra ~600 KB buys full-colour
drawing with the ink decision made at the last moment.** It's also why PSRAM is
mandatory: 720 KB can't fit in ~320 KB of internal RAM (a 4-bit buffer could
have).

### Measured (`stages/lesson2_psram_check.cpp`)

Free memory snapshotted before and after `M5.begin()`:

```
                 before      after      drop
PSRAM free       8386096    7663840    722256
PSRAM largest    8257524    7602164    655360
internal free     336620     326728      9892
internal large    278516     278516         0
predicted frame buffer :   720000  (400 x 600 x 3)
display 400 x 600, colour depth reported: 24 bits
```

- **PSRAM dropped 722,256 vs 720,000 predicted** — 99.7 % is the frame buffer. The
  other 2,256 bytes are allocator overhead or other small allocations; this
  measurement can't say which, so they stay unattributed.
- **24-bit colour depth confirmed** by the driver itself.
- **Internal RAM −9,892**: includes the 2,400-byte row-pointer table, plus
  M5Unified's other setup. Largest internal block unchanged → no fragmentation.
- **"PSRAM largest" fell 655,360, not 722,256** — that metric describes *where the
  gaps are*, not how much was used. What matters: 7.6 MB of contiguous PSRAM is
  still free. (Gotcha 36's free-vs-largest distinction, from the other side.)

---

## Part 4 — Dithering

### The idea

```python
bias = pattern[y % 16][x % 16]          # fixed, position-dependent nudge
ink  = nearest_ink(r + bias, g + bias, b + bias)
```

The same colour is nudged up at some positions and down at others, so across an
area it lands on different inks in proportion to how close it is to each — your
eye averages the dots.

### The four modes (`_exec_transfer`, `.inl:434`)

| `epd_mode` | Function | Strength | Pattern |
|---|---|---|---|
| `epd_fastest` | `_dither_row_none` | — | nearest ink only |
| `epd_fast` | `_dither_row_bayer_simple_pair` | 70 | Bayer 16×16; same nudge on R, G, B |
| `epd_text` | `_dither_row_rgb_pair` | 70 | arithmetic pattern; different nudge per channel |
| `epd_quality` | `_dither_row_rgb_pair` | **140** | same, twice as strong — **the driver's default** |

So Lesson 01's `setEpdMode(epd_fastest)` *turned dithering off*; without it our
flat swatches would have been dithered at full strength.

### Bayer (`epd_fast`, `.inl:162`)

```cpp
auto row = &bayer256[(y & 15) << 4];      // row y%16 of a flat 16×16 table
int32_t bias = row[x & 15];               // 0…255 by position
bias = bias * 2 - 255;                    // → −255…+255
bias = bias * dither >> 8;                // × 70/256 → −70…+69 (measured)
```

| C++ | Python | Why |
|---|---|---|
| `x & 15` | `x % 16` | for powers of two, a mask equals the remainder — and is cheaper |
| `(y & 15) << 4` | `(y % 16) * 16` | start of row *y* in the flat table |
| `* dither >> 8` | `* dither // 256` | scale without dividing |

`bayer256` holds each value 0–255 exactly once, ordered so any threshold lights a
spread-out set of positions, never a clump — hence the cross-hatch texture.

### Pair matching — the clever part (`_rgb_to_epd_color_pair`, `.inl:95`)

All dithered modes choose inks for **two neighbouring pixels at once**, scoring
all 36 combinations:

```
score = (error_left + error_right)²     ← block: does the PAIR average to the target?
      + error_left² + error_right²      ← individual: is each pixel close on its own?
```

The comment says the individual term **keeps hue** — without it, "dusty orange"
could collapse to black + white. The simulator agrees: dusty orange (200, 130, 90)
→ **YELLOW + RED**.

**Its side effect on mid-grey (128, 128, 128):**

| Pair | Block | Individual | Score |
|---|---|---|---|
| BLACK + WHITE — averages to *exactly* grey | **3** | 97,539 | 97,542 |
| GREEN + GREEN | 55,284 | 27,642 | 82,926 |
| **BLUE + GREEN ← chosen** | 11,566 | 34,830 | **46,396** |

Black + white is a near-perfect average but each dot is individually far from
grey, so it is heavily penalised. Blue + green averages to a **teal** (~84, 101,
142). The term that keeps orange orange is what turns grey blue-green — a real
trade-off, of which the code comment mentions only the benefit.

### The text/quality pattern (`.inl:188`)

No lookup table: a counter steps by fixed amounts per pixel (127×29) and per row
(129×48), wrapping modulo 16,383, and three **phase-shifted** copies give each
channel its own nudge. Measured from the port: `epd_text` ±18 shared + ±18 per
channel; `epd_quality` ±35 shared + ±35 per channel. Per-channel nudges can mix inks
of *different hues*, not just lighter and darker — suited to colour images.

### The simulator (`tools/epd_sim.py`)

A pure-Python, line-for-line port of all four paths, integer maths included
(Python's `>>` floors negative numbers the same way GCC's arithmetic shift does).
It reads `bayer256` **straight from the driver source**, so the two can't drift.

```bash
python3 tools/epd_sim.py          # → sim/source.png, fastest.png, fast.png,
                                  #   text.png, quality.png, compare.png (≈6 s)
```

Test image (400 × 600), identical maths to be used on the device in Part 5:

| Band | Rows | Content |
|---|---|---|
| A | 0–99 | grey ramp |
| B | 100–199 | full-saturation rainbow |
| C | 200–299 | 8 problem patches: mid/light/dark grey, dusty orange, pink, purple, cyan, navy |
| D | 300–599 | hue × lightness (white at the top → black at the bottom) |

What it predicts:

- **fastest:** hard-edged blocks; grey ramp black → green → blue → white; mid-grey
  patch green, cyan blue, purple red, navy black.
- **fast:** smooth ramps with a visible cross-hatch.
- **text / quality:** smoothest, diagonal texture; orange, pink, purple, navy look
  like themselves.
- **Greys tinted in every mode.** Ink share across the grey ramp:

| Mode | GREEN | BLUE | WHITE | BLACK | YELLOW |
|---|---|---|---|---|---|
| fastest | 37 % | 7 % | 34 % | 20 % | — |
| fast | 32 % | 11 % | 33 % | 23 % | — |
| text | 27 % | 18 % | 25 % | 21 % | 7 % |
| quality | 28 % | 16 % | 26 % | 21 % | 6 % |

**Caveat:** the simulator paints each ink with the driver's *assumed* RGB, not a
photo of the real ink.

---

## Part 5 — Verify on the glass

### Method (`stages/lesson2_glass_vs_sim.cpp`)

`pixel()` from the simulator was ported to C++ with the **same integer maths**
and drawn into the frame buffer **once** (240,000 `drawPixel` calls in one
`startWrite()`/`endWrite()` batch: **204 ms**). A/B choose a mode, C refreshes.

Two earlier findings made this cheap:

- **No redraw per mode.** The frame buffer keeps full RGB (Part 3), so the ink
  decision is made at `display()` time — `setEpdMode()` then `display()` is enough.
- **…except that `display()` skips the refresh if nothing was drawn** (Part 1). So
  each commit redraws a small `epd_mode: …` label at the bottom, which marks the
  buffer dirty. Without it, a mode change would silently do nothing.

### Predictions vs the glass

| Mode | Prediction (simulator) | Seen on the glass |
|---|---|---|
| fastest | six solid inks; grey ramp black → green → narrow blue → white; mid/dark grey green; navy black; orange → yellow | **All confirmed** |
| fast | smooth-ish ramp with a blue-green tint; cross-hatch texture; dusty orange as yellow + red | **Confirmed**: "kind of smooth with a blue-green tint"; dot pattern "still noticeable for some colours at arm's length"; orange looks orange — *best-looking oranges of the four* |
| text | diagonal texture; ramp with some yellow (7 %) | **Confirmed, plus a surprise**: diagonal and *more* noticeable; ramp in distinct zones black → greenish → bluish → **yellowish** → white; **blotchier** than fast |
| quality | smoothest; greys still tinted | **Confirmed**: smoother, slight tint; finer texture but still noticeable; **best match to the source** in the hue × lightness band |

The 7 % yellow the simulator predicted for `text` (and not for fastest/fast) was
visible on the glass — a small detail of the model, confirmed.

**Why `text` is blotchier than `fast`:** at strength 70 the arithmetic pattern
nudges only about ±18 shared + ±18 per channel, versus Bayer's ±70. A weaker nudge
mixes inks over narrower ranges, and between them colours snap to one ink — zones
and blotches. The likely intent (our reading; the code doesn't say): a weak nudge
keeps **text and line edges crisp**, without stray dots around glyphs. "text" means
*tuned for different content*, not *worse*.

### Cost of dithering — measured

| Mode | `display()` | vs fastest |
|---|---|---|
| fastest | 16,151 ms | — |
| fast | 16,718 ms | +567 ms |
| text | 16,757 ms | +606 ms |
| quality | 16,757 ms | +606 ms |

Pair matching (36 ink pairs for each of 120,000 pixel pairs) costs ~0.6 s of CPU.
`text` and `quality` take *identical* time — same function, only the nudge
strength differs, which costs nothing. Still only ~4 % of a refresh; the panel
dominates (Part 2).

### Do exact inks survive dithering? (simulator)

Flat areas drawn in the six exact palette colours stay **100 % pure** in every
mode for black, white, yellow, red and blue; green is 97–100 %, and `quality`
sprinkles 2 % stray dots into black. So for a UI built from exact inks, the mode
barely matters — it matters for **in-between** colours: photos, gradients, and the
anti-aliased edges of smooth fonts (not yet tested on the glass).

### Choosing a mode — what we concluded

- **The mode applies to the whole screen, per refresh.** You can't have `text`
  for the labels and `quality` for a photo in one refresh.
- **Dashboard of text + icons in exact inks:** `fastest` — crisp, no stray dots,
  0.6 s quicker. (If smooth/anti-aliased fonts are used, `text` is the candidate —
  untested.)
- **Photo:** `quality` matched the source best; `fast` gave the most pleasing
  oranges. Taste, and content, decide.
- **A design idea for mixed screens:** dither the *photo region yourself* (in app
  code, using only the six exact inks) and refresh the whole screen in `fastest`.
  Exact inks pass through untouched, so the text stays crisp and the photo keeps
  your chosen dithering. (This is what writing our own low-level code would buy.)

---

## What you learned

- ✅ Read a vendor driver; separated known / likely / unknown in its init table
- ✅ Measured a refresh phase by phase with an ISR on a pin you don't own (91 %
  panel, 7.4 % deliberate waits, 1.5 % transfer)
- ✅ Found an unchecked timeout with 28 % headroom — a testable cold-weather risk
- ✅ Derived, corrected, then measured the frame-buffer cost (722,256 B, 24-bit)
- ✅ Nearest-colour matching and why grey → green
- ✅ Bayer and arithmetic dithering; pair matching and its grey side effect
- ✅ Built a host simulator to predict before spending hardware time
- ✅ Verified every simulator prediction on the glass; found why `text` bands
- ✅ Measured dithering's CPU cost (~0.6 s) and chose modes by content

## Files

| File | What |
|---|---|
| `stages/lesson2_busy_timeline.cpp` | BUSY-edge ISR + on-screen timing report |
| `stages/lesson2_psram_check.cpp` | heap/PSRAM before vs after `M5.begin()` |
| `stages/lesson2_glass_vs_sim.cpp` | test image on the glass, A/B/C through the four modes |
| `tools/epd_sim.py` | host simulator of the four dither paths |
| `sim/*.png` | simulator output (regenerate any time) |

## Glossary

- **Init table** — a byte list of `[command, count, data…]` sent at power-up.
- **BUSY pin** — LOW while the panel works; the driver polls it.
- **ISR** — interrupt service routine: a tiny function the hardware calls on an event.
- **`IRAM_ATTR` / `volatile`** — "keep this in internal RAM" / "always re-read this".
- **Nibble** — 4 bits; two per byte.
- **Ordered dithering** — a fixed, position-based nudge pattern (Bayer is the classic).
- **Pair matching** — choosing inks for two pixels together, scoring their average
  and each one individually.
- **Free vs largest block** — how much memory is free vs the biggest single piece.

## Next lesson

Candidates (see `docs/PLAN.md`): the **SHT40 sensor + RTC**, including the
cold-refresh experiment (does cold eat the 28 % timeout headroom?); then a
**deep-sleep dashboard**. Optional exercise from Part 5: dither a photo region
yourself and refresh in `fastest`.

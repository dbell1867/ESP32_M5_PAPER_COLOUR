# Lesson 07 — Photos in Flash, and the SD Card as an Option

**Project:** M5Stack PaperColor (SKU C151) in C++ with PlatformIO
**Goal of this lesson:** Keep the photos (fewer than 20, shown on demand) in the board's own
flash memory, so the photo frame works with no card — and bring the SD card back as an
optional second source, with a working card-detect.

> Audience note: Python / CircuitPython background, learning C++.
> **Status:** complete. Firmware: `stages/lesson7_card_detect.cpp` (= `src/main.cpp`).
>
> **Privacy:** as in Lesson 06, photos and their converted files live only in the
> git-ignored `images/` folder — including the new flash staging folder `images/fs/`.

---

## Learning objectives

1. Read a **partition table**: what lives where in the 16 MB flash.
2. Put files on a **flash filesystem** (LittleFS) and upload it without touching the rest.
3. Choose between NVS and a filesystem for different kinds of data.
4. Use a pin that isn't on the ESP32 at all: card-detect through the **power chip**.
5. Re-test an old claim properly — and correct it.

---

## Design decisions (made before any code)

| Question | Decision | Why |
|---|---|---|
| Where do photos live? | **Internal flash**, SD optional | always there; nothing to mount or power; teaches partitions |
| How many? | < 20 (120,000 bytes each) | fits the existing 3.4 MB data partition — no repartitioning |
| How do they get there? | `pio run -t uploadfs` over USB | simple; Wi-Fi upload could come later |
| What if both have a photo? | **flash first**, then SD | flash is the primary, curated set |
| Mount at boot? | **no — only for `PHOTOS` / `SHOW`** | ~650 routine wakes a day shouldn't pay for it |
| Empty/corrupt filesystem? | **never auto-format**; say "run uploadfs" | an accident shouldn't erase anything |

## Part 1 — Photos in flash (LittleFS)

### What's in the 16 MB flash

`board_build.partitions = default_16MB.csv` (from the Arduino core):

| Name | Type | Offset | Size | Holds |
|---|---|---|---|---|
| (bootloader, partition table) | — | 0x0000 | 36 KB | start-up code, this table |
| `nvs` | data | 0x9000 | 20 KB | settings + logs: Wi-Fi, battery/sync/refresh logs |
| `otadata` | data | 0xE000 | 8 KB | which app slot to boot |
| `app0` / `app1` | app | 0x10000 / 0x650000 | 6.25 MB each | the firmware (ours: 1.3 MB = 20 %) |
| `spiffs` | data | 0xC90000 | **3.375 MB** | **the photos** (LittleFS, despite the name) |
| `coredump` | data | 0xFF0000 | 64 KB | crash dumps |

**NVS vs filesystem.** NVS is a key–value store for small, frequently rewritten values (it
spreads writes to protect the flash). A filesystem holds **files**: a 120 KB photo is one
file, read row by row. Rule of thumb: settings and counters → NVS; content → files.

### Host side

- `platformio.ini`: `board_build.filesystem = littlefs`, and `data_dir = images/fs` — the
  folder `uploadfs` builds the image from, **inside the git-ignored `images/`**.
- `tools/stage_photos.py` copies the **existing** `images/<name>/<name>.epd4` files into
  `images/fs/photos/` (it doesn't re-convert: re-running `photo_prep.py` with default
  settings could change crops you'd approved), checks each is 120,000 bytes, and reports
  how full the partition will be.

**Checked before uploading anything:** `pio run -t buildfs` made a **3,538,944-byte
(0x360000)** image, and the upload command writes it at **13172736 = 0xC90000** — exactly
the data partition. NVS and the firmware can't be touched. (After both uploads, the
Wi-Fi settings were confirmed intact.)

### Firmware

```cpp
#include <LittleFS.h>
LittleFS.begin(false /*formatOnFail*/, "/littlefs", 5, "spiffs");   // partition label
```

`showPhoto()` now takes the file from whichever filesystem has it — both `LittleFS` and
`SD` are an `fs::FS`, so the drawing code doesn't care:

```cpp
fs::FS* src = nullptr;
if (fsMount() && LittleFS.exists(path)) src = &LittleFS;
else if (sdMount() && SD.exists(path))  src = &SD;
File f = src->open(path);          // ...same row-by-row drawing as Lesson 06
```

### Result

```
Wrote 3538944 bytes (124700 compressed) at 0x00c90000 in 8.1 seconds — Hash of data verified.
# /photos in flash
  disney.epd4   120000
  dog.epd4      120000
  flash: 262144 of 3538944 bytes used
photo: /photos/dog.epd4 from flash shown via route B (load 208 ms, refresh 15986 ms)
```

- Mostly empty, the 3.4 MB image compresses to 125 KB, so the upload takes 8 s.
- Two photos use 262,144 bytes: 240,000 of data + ~22 KB of filesystem overhead. About 26 fit.
- Loading from flash: **208 ms** (from SD in Lesson 06: 258 ms).

**Workflow:**

```bash
python3 tools/photo_prep.py ~/Downloads/new.jpg [--x --y --zoom]   # convert; check compare.png
python3 tools/stage_photos.py                                      # stage all converted photos
# wait for a service window (tools/wait_window.py), then:
pio run -t uploadfs --upload-port /dev/ttyACM0
```

An upload replaces the **whole** photo partition with what's staged — the staging folder is
the master list.

## Part 2 — The SD card as an option, with a card-detect

### Card-detect lives in the power chip

M5Stack's docs list ESP32 **G1** as card-detect; in Lesson 01 it never changed with the card
(it's button C). M5Stack's factory firmware shows the real wiring: the slot's switch goes to
the **M5PM1's GPIO1** (`SD_DEC`), with its **GPIO4** (`SD_DET_EN`) as an enable. From the
M5PM1 datasheet:

| Register | Use | Set |
|---|---|---|
| `0x16` GPIO_FUNC0, bits 3:2 | GPIO1 function | `00` = GPIO (default) |
| `0x10` GPIO_MODE | 1 = output | GPIO1 input, GPIO4 output |
| `0x14` GPIO_PU/PD_0, bits 3:2 | GPIO1 pull | `01` = pull-up |
| `0x13` GPIO_DRV | 0 = push-pull | GPIO4 push-pull, `0x11` bit 4 HIGH |
| `0x12` GPIO_IN | live levels | bit 1: **LOW = card** |

Every standby power-off resets these (`HOLD_CFG = 0`), so `cardDetect()` sets them up each
time — cheap: four I²C writes and a 5 ms settle. Without the pull-up the pin reads LOW with
no card (Lesson 06's register dump: `GPIO_IN = 0x0D`), so the pull-up isn't optional.

| | GPIO_IN | GPIO1 | `CARD?` says |
|---|---|---|---|
| card out | `0x1F` | HIGH | no card ✓ |
| card in | `0x1D` | LOW | card PRESENT ✓ |

`sdMount()` now returns at once on a definite "no card" — before, it tried 6 times at three
speeds and printed a wall of `sdCommand(): Card Failed!` errors, costing ~4 s.

### Re-testing an old claim: does the card slow the panel?

Lesson 06 recorded "card in → transfer 241 → 639 ms, BUSY glitches". Re-tested properly —
card inserted, **mounted and listed**, then refreshes in the **middle** of a wake:

```
dashboard (boot)  transfer 241 ms, busy 131 / 14,226 / 152 ms
REDRAW            transfer 241 ms, busy 131 / 14,227 / 152 ms
REDRAW            transfer 241 ms, busy 131 / 14,228 / 152 ms
```

**It doesn't.** Looking back, every 639 ms reading was the **first refresh after an upload,
from before the panel-reset fix** (Lesson 06 3g) — with and without a card — and since the
fix first refreshes show 241 ms too. So they were most likely another symptom of the
un-reset controller. (Likely, not proven: the un-reset state can't be recreated without
removing the fix.) Lesson 06 is corrected.

> **Method note:** a claim that rests on readings taken under one special condition (here:
> the first refresh after a restart) isn't about what you thought it was. When re-testing,
> change *only* the variable in question — card in vs out — and avoid the special case.

## Command cheat-sheet

| Command | What |
|---|---|
| `PHOTOS` | list `/photos` in flash (with space used) and on the card (if present) |
| `SHOW <name>` | show `<name>.epd4` — flash first, then SD; holds it until the power button |
| `SHOW <name> A` | route A: `<name>.jpg`, dithered by the driver (only on the card at present) |
| `CARD?` | card-detect: raw `GPIO_IN` and the verdict |

## Files

| File | What |
|---|---|
| `tools/stage_photos.py` | copy converted `.epd4` files into `images/fs/photos/` (git-ignored) |
| `platformio.ini` | `data_dir = images/fs`, `board_build.filesystem = littlefs` |
| `stages/lesson7_flash_photos.cpp` | Part 1: photos from LittleFS, flash-first `SHOW` |
| `stages/lesson7_card_detect.cpp` | Part 2: + card-detect via the M5PM1 (= `src/main.cpp`) |

## Glossary

- **Partition table** — the map of the flash: name, type, offset and size of each region.
- **LittleFS** — a small filesystem designed for flash (power-loss safe, wear-levelling).
- **`uploadfs`** — PlatformIO target that builds a filesystem image from `data_dir` and
  writes it to the data partition.
- **Card-detect** — a switch in the card slot that closes when a card is inserted.
- **Pull-up** — a resistor that holds an input HIGH unless something pulls it LOW.

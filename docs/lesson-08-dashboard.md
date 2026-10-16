# Lesson 08 — A Dashboard for the House: Weather, Pages and a Wi-Fi QR Code

**Project:** M5Stack PaperColor (SKU C151) in C++ with PlatformIO
**Goal of this lesson:** Turn the test dashboard into something genuinely useful at home:
date, room temperature and humidity, **today's weather and the next 5 hours**, when it was
updated and how much battery is left — plus **pages** stepped through with a button: the
dashboard, a **Wi-Fi login QR code**, and the photos from Lesson 07.

> Audience note: Python / CircuitPython background, learning C++.
> **Status:** complete. Firmware: `stages/lesson8_nvs_fix.cpp` (= `src/main.cpp`).
>
> **Privacy:** the Wi-Fi password is in the QR code. It is built on the board from the
> credentials already in NVS, and is never printed, logged, screenshotted or committed.
> The weather location is set on the board with `LOC` and isn't in git either.

---

## Learning objectives

1. Call a **web API over HTTPS** from a microcontroller — and actually verify the server.
2. Parse JSON with **ArduinoJson** and keep only what you need, in a fixed-size struct.
3. **Schedule** network work on a board that is off most of the time.
4. Split a big `main.cpp`: **drawing** in its own file, fed by one plain struct.
5. Draw weather **icons from primitives** using only the panel's six inks.
6. Review layouts **on the computer** instead of waiting for the panel (`SHOT`).
7. Design a **multi-page UI** around the one button that can wake the board.

---

## Design decisions (made before any code)

| Question | Decision | Why |
|---|---|---|
| Weather source | **Open-Meteo** | free, no API key or account, small JSON answers |
| Location | London, set on the board (`LOC <lat> <lon>`) | not in git; easy to change |
| How often? | weather **hourly**, room every **10 min** | the forecast barely changes faster; Wi-Fi costs battery |
| Low battery? | **no weather fetches** | Wi-Fi current peaks are the likeliest brownout (Lesson 05) |
| Which button steps pages? | the **power button** | the only one that can wake the board from L1 standby |
| How long does a page stay? | **until the next press** | a photo or QR code shouldn't vanish after 10 min |
| QR for which network? | the main network (no guest network yet) | the router can't isolate guests today |

## Part 1 — Planning: what costs what

Every wake already costs a ~14 s panel refresh at most every 10 min. Adding Wi-Fi to *every*
wake would be the biggest drain on the battery, so the network is used **at most once per
wake** and only when something is due: the clock sync (daily) and the weather (hourly) share
one connection when both are due.

## Part 2 — Weather over verified HTTPS

### The request

One URL asks for exactly what the screen shows (~830 bytes back):

```
https://api.open-meteo.com/v1/forecast?latitude=..&longitude=..
  &timezone=Europe%2FLondon&timeformat=unixtime&forecast_days=1
  &daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max
  &hourly=weather_code,temperature_2m,precipitation_probability,is_day
  &forecast_hours=7
```

- `forecast_hours=7` — the current hour + 6, so "the next 5 hours" is still full when the
  next fetch is up to an hour late.
- `timeformat=unixtime` — times as plain numbers; no date-string parsing on the board.
- `is_day` — whether to draw a sun or a moon for each hour.

### Verifying the server: certificate pinning to two roots

`NetworkClientSecure` can either check the server's certificate against roots you give it,
or skip the check (`setInsecure()`). Skipping it means anyone between the board and the
server could answer instead — so we **never** do that.

Open-Meteo's certificate comes from Let's Encrypt, so `include/isrg_roots.h` holds two
public root certificates:

- **ISRG Root X1** — today's chain ends here;
- **ISRG Root YR** — Let's Encrypt's newer root, so the board keeps working when the chain
  moves to it.

Each was tested **on its own** with `curl --cacert` against the real server before going in
(both pass; ISRG Root X2, the ECDSA root, fails — it isn't in Open-Meteo's chain).

> **Why HTTPS needs the clock:** certificates have validity dates. With a wrong clock (a
> flat RTC battery) every certificate looks expired or not-yet-valid. So the fetch only
> happens once the clock is valid — after the NTP sync if one was due in the same wake.

### Parsing: keep the answer small

`Weather::fetch()` (`src/Weather.cpp`) parses the JSON with **ArduinoJson 7** and copies the
few numbers we need into a fixed 43-byte struct:

```cpp
struct __attribute__((packed)) WxData {
  uint32_t fetchedAt;         // when (0 = no data)
  uint8_t  dayCode;           // today: WMO weather code
  int16_t  dayMax_c10, dayMin_c10;   // tenths of a degree: 15.2 C -> 152
  uint8_t  dayRain;           // %
  uint32_t hour0;             // time of the first hourly entry
  uint8_t  code[7]; int16_t temp_c10[7]; uint8_t rain[7];
  uint8_t  dayMask;           // bit i: hour i is daytime
};
```

It goes into NVS (namespace `wx`, key `d`) as one blob, like the logs in Lesson 04.
**Temperatures in tenths, as integers** keep it small and exact; `packed` stops the compiler
inserting padding, so the size on flash never changes by surprise.

> **Python comparison:** `json.loads()` gives you a dict of everything. On a microcontroller
> you'd rather pull out the 30 numbers you need and throw the rest away straight after —
> ArduinoJson's `JsonDocument` is freed when `fetch()` returns.

### The schedule

In `setup()`, after reading the battery:

```cpp
bool wxNow = haveCreds && !lowBatt && wxLocation(lat, lon) &&
             now - wd.fetchedAt >= 3300 &&     // >= 55 min since the last good fetch
             now - wxTry >= 900;               // >= 15 min since the last attempt
```

- **55 min, not 60:** wakes are every 2 min, so "≥ 60" would often mean 62 — and drift later
  each hour.
- **The attempt time is saved before trying**, so a failing server or network costs one
  try per 15 min, not one per wake.
- If the forecast is more than 3 hours old, the footer says so: `(weather 09:35)`.

**Result:** connecting to Wi-Fi takes ~4 s; the HTTPS fetch itself ~0.5 s. The values matched
Open-Meteo's own website.

## Part 3 — The new dashboard (`src/Screens.cpp`)

### Splitting drawing from everything else

`main.cpp` had grown past 1,300 lines. Drawing now lives in `Screens.cpp`, which knows
nothing about sensors, NVS or Wi-Fi. `main.cpp` gathers the facts into one struct and hands
it over:

```cpp
struct DashInfo {
  time_t now;  float roomC, roomRH;  float battV;
  bool usb, lowBatt, haveWx;  WxData wx;
};
void drawDashboard(const DashInfo& d);
```

This makes the drawing easy to test (see `SHOT` below) and easy to change without touching
the power logic.

### Layout (400 × 600, top to bottom)

| Section | Shows |
|---|---|
| Date | `Sunday 11 October` (bold 18 pt; 12 pt if a long day + month won't fit) |
| INDOORS | room temperature with **C** (blue < 18, red > 25), relative humidity |
| TODAY | icon, description ("overcast"), **red max** / **blue min**, chance of rain |
| Next 5 hours | 5 columns: hour, icon, temperature, rain % (only if > 0; blue from 30 %) |
| Footer | `Updated 12:40`, battery **%** and a battery icon (with a bolt on USB) |

The fonts have no degree sign, so `drawTemp()` draws a small ring before the C.

### Icons from circles and lines

There's no icon font, and bitmaps would have to be converted for the six inks. Instead each
icon is a few primitives — a sun is a yellow circle with lines, a cloud is overlapping white
circles with a black outline, rain is short blue lines, snow small crosses, thunder a yellow
bolt. `wxIcon(code, day, cx, cy, size)` picks from the WMO weather code. They scale to any
size, so today's big icon and the hourly small ones are the same code.

All colours are **exact panel inks** (`INK_RED = 0xBF0000` etc., Lesson 02): anything else
would be dithered into a speckled pattern.

**Yellow needs an outline.** Yellow is the palest ink: a yellow sun on white was hard to see
on the glass. The sun, moon and thunder bolt now get a black outline (like the cloud: the
shape a little bigger in black, then yellow on top) — 1 px on the small hourly icons, 2 px
on the big one.

**…and no anti-aliasing.** The first outlined version used `drawWideLine` for the rays. The
icon-sheet screenshot (`SHOT ICONS`, below) showed its soft edges: blended grey/olive pixels
that the panel prints as **green** — 1,824 green pixels on the sheet. The rays are now pairs
of filled triangles and the fog bars rounded rectangles, every pixel exactly one ink: 0
green pixels. Rule: on a six-ink panel, draw with **filled shapes**, not smoothed lines.

**…and even weight (16 Oct, checked on the glass).** The outlines looked lighter on the
straight rays than on the diagonals. In the pixel map both are 1 px, but a diagonal edge is
a staircase (`##` steps), so it puts down about twice the black. The four straight rays now
get 1 px more outline on their sides only (the ends unchanged, keeping the gap to the disc):
small suns 2 px, big sun 3 px. On the panel the outlines now look even.

### Battery percentage

A LiPo's voltage isn't linear in charge: it drops fast near full, sits around 3.7–3.9 V for
most of the charge, then falls steeply. `batteryPercent()` interpolates a typical
single-cell curve, from **3.30 V = 0 %** (our shutdown voltage, Lesson 04) to **4.20 V =
100 %**. It's approximate — and on USB it reads as charging, not resting, so the icon gets a
bolt.

### Reviewing a layout without the panel: `SHOT`

Each try on the glass costs 14–25 s plus a button press. `SHOT` draws the dashboard into
the frame buffer **without** a refresh and streams it over serial as hex RGB rows;
`tools/screenshot.py` saves two PNGs in the git-ignored `images/screens/`:

- `-buffer.png` — what the firmware drew;
- `-glass.png` — after the driver's `epd_fastest` ink mapping (via `tools/epd_sim.py`), i.e.
  what the panel will actually show.

`SHOT ICONS` (`tools/screenshot.py icons`) does the same for a sheet of every weather icon
at both sizes — the forecast rarely shows them all at once.

Several layout rounds took minutes instead of an afternoon, and the one change you asked for
(the **C** after the room temperature) was checked on screen before flashing.

## Part 4 — Pages and the power button

### Why the power button

In L1 standby the ESP32 is **completely off**; only the M5PM1 power chip is alive. The
ESP32's buttons (A/B/C) can't wake anything. The M5PM1 watches just two things: its timer
and the **power button**. So the power button is the only button that works whenever you
press it.

| Press | What the M5PM1 does |
|---|---|
| single | power on → our firmware sees `WAKE_SRC` bit `0x04` → **next page** |
| double | power off (M5PM1 built-in) |
| long, ~3 s | download mode (M5PM1 built-in) |

### The page cycle

```
dashboard -> Wi-Fi QR -> photo 1 -> photo 2 -> ... -> dashboard
```

- The current page is one byte in NVS (`dash/page`): 0 dashboard, 1 QR, 2+ photos (the flash
  photos in alphabetical order), 255 a photo put up with `SHOW`.
- **Non-dashboard pages stay up until the next press**: timer wakes still read the sensor,
  log the battery and fetch the weather, but don't refresh the panel.
- **Low battery or a fresh boot always returns to the dashboard** — its warnings matter
  more than a photo.
- Plugging in USB opens a service window but **doesn't** step the page.
- If a page can't be shown (no Wi-Fi set up, a photo missing) the board falls back to the
  dashboard rather than leaving the old picture.

### The Wi-Fi QR code

Phones understand a standard text format in a QR code:

```
WIFI:T:WPA;S:<network name>;P:<password>;;
```

with `\ ; , : "` escaped by a backslash (`qrEscape()`). The board builds this text from the
credentials `tools/wifi_setup.py` stored in NVS (Lesson 05), draws it with M5GFX's
`qrcode()` 360 px wide — large enough for any phone camera at arm's length — and then
clears the string. Nothing about it reaches the serial port, the logs or a screenshot.

**Result:** scanning with a phone's camera joined it to the network; the full cycle
dashboard → QR → disney → dog → dashboard worked.

## Part 5 — Cleaning up

The Lesson 06 experiments are finished, so their commands are gone from the firmware:
`TSWEEP`, `TFSWEEP`/`TFRES` (forced panel temperature), `SDDET` and `PMDUMP` (power-chip
state), `REDRAW`. Their code is kept in `stages/lesson6_*.cpp`. The header of `main.cpp`,
still describing Lesson 04, now describes the current firmware and lists every command.

## Part 6 — Three days later: the weather stopped (NVS full)

**Symptom:** after almost 3 days the hourly weather stopped changing.

**Evidence first** (USB plug-in → service window, no page change; `LOG`, `WEATHER`, `RLOG`):

```
weather: OK in 470 ms ...                               <- the fetch WORKED
putBytes(): nvs_set_blob fail: d NOT_ENOUGH_SPACE       <- but couldn't be saved
putUInt():  nvs_set_u32 fail: try NOT_ENOUGH_SPACE
weather: fetched 2026-10-14 00:31 (484 min ago)         <- the last save that fitted
# NVS entries: used 504, free 126, total 630
```

NVS (the settings store, Lesson 04) is a 20 KB partition: 5 pages × 126 entries of 32
bytes, with one page always kept free for reorganising — **504 usable entries**. It was
completely full. The battery log had stopped at entry 60 (00:46), the refresh log on 12 Oct.

**Why:** each log record was its own key, and every key costs overhead — a 10-byte
battery record took **3 entries** (blob index + chunk header + data). At full size the logs
alone needed ~572 entries: more than all of NVS. A fresh boot clears the battery log, which
hid the problem; after Lesson 08's restarts it grew for ~62 hours (186 entries) into what
the refresh log and everything else had left.

**A second problem hiding behind it:** the weather's "last attempt" time couldn't be saved
either, so every 2-minute wake thought the weather was due and **joined Wi-Fi** — about 240
extra connections overnight, on battery.

**The fix:**

| | Before | After |
|---|---|---|
| Log format | one key per record | records **packed into blobs**, one key per blob (`ringAppend` / `ringForEach`) |
| Refresh log | 192 records, 122 entries | 48–64 records, 42 entries (the investigation is over) |
| Battery log (120 h) | 360 entries | 67 entries |
| Sync log (30 days) | 90 entries | 29 entries (history migrated) |
| Attempt time can't be saved | tries anyway, every wake | **doesn't try** (sync and weather) |
| Forecast can't be saved | silent | `weather: fetched but NOT saved (NVS full)` |
| Running low | invisible | `LOG` prints NVS use; footer shows **STORAGE FULL** below 40 free |

**Result:** `used 200, available 304` straight after the migration; the 9 sync records came
across intact; the weather saved at 08:45. At full size the logs now need ~138 entries.

**Two days later (16 Oct, `docs/data/battery-2026-10-16.csv`):** `used 262, available 242`.
The growth (+62 entries) matches the budget exactly: refresh log now full (+30), battery log
49 hours in 5 blobs (+24), sync log 11 records in 2 blobs (+7). Still to grow: battery log
to 11 blobs (+36), sync log to 4 (+14) → about **312 used, ~190 available** for good. The
battery log is unbroken (49 hourly rows), the clock synced daily, the weather was 17 min
old, and the battery fell ~0.9 mV/h (4.126 → 4.087 V in 42 h) — no sign of the Wi-Fi storm.

> **Lesson:** a log that runs for days needs a **storage budget on paper** — entries per
> record × records kept, for every log, against what the store really holds (here 504, not
> 630) — and a write that fails must change behaviour (and be visible), never just be
> ignored. Also: the firmware printed the Wi-Fi network name on every connect, and it ended
> up in a captured log. It no longer prints it.

## Command cheat-sheet (new in this lesson)

| Command | What |
|---|---|
| `LOC <lat> <lon>` | set the weather location (stored on the board) |
| `WEATHER` | show the stored forecast and when it was fetched |
| `WXNOW` | fetch the weather now |
| `SHOT` | stream the dashboard's frame buffer (use `tools/screenshot.py`) |
| `SHOT ICONS` | the same for a sheet of every weather icon (`tools/screenshot.py icons`) |

## Files

| File | What |
|---|---|
| `src/Weather.h/.cpp` | `WxData`, `Weather::fetch()` (HTTPS + JSON), `describe()` |
| `include/isrg_roots.h` | ISRG Root X1 + Root YR (public certificates) |
| `src/Screens.h/.cpp` | `drawDashboard()`, `drawWifiQr()`, icons, `batteryPercent()` |
| `tools/screenshot.py` | `SHOT` → `images/screens/*-buffer.png` and `*-glass.png` |
| `stages/lesson8_weather.cpp` | Part 2: hourly weather in NVS |
| `stages/lesson8_dashboard.cpp` | Part 3: the new layout |
| `stages/lesson8_pages.cpp` | Part 4: pages and the QR code |
| `stages/lesson8_final.cpp` | Part 5: experiments removed |
| `stages/lesson8_nvs_fix.cpp` | Part 6: packed NVS logs, save guards, storage warning (= `src/main.cpp`) |

## Glossary

- **API** — a web address that returns data for programs (here JSON) instead of a page.
- **Root certificate** — a certificate your device trusts by itself; a server's certificate
  is accepted if it chains up to one of them.
- **Certificate pinning** — trusting only specific roots, rather than a whole store of them.
- **WMO weather code** — a standard number for the weather (0 clear, 3 overcast, 61 rain...).
- **Frame buffer** — the picture in memory that a refresh sends to the panel.
- **Packed struct** — a struct with no padding bytes, so its layout is exactly as written.

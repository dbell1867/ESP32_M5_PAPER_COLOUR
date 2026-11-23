# Lesson 08 — A Dashboard for the House: Weather, Pages and a Wi-Fi QR Code

**Project:** M5Stack PaperColor (SKU C151) in C++ with PlatformIO
**Goal of this lesson:** Turn the test dashboard into something genuinely useful at home:
date, room temperature and humidity, **today's weather and the next 5 hours**, when it was
updated and how much battery is left — plus **pages** stepped through with a button: the
dashboard, a **Wi-Fi login QR code**, and the photos from Lesson 07.

> Audience note: Python / CircuitPython background, learning C++.
> **Status:** complete. Firmware: `stages/lesson8_charge_log.cpp` (= `src/main.cpp`; Parts 8-9, 22-23 Nov).
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

> **Changed 30 Oct (Part 7):** the power button now only opens a window; side buttons A/B
> step the pages in it. The reasoning below still explains why the power button comes first.

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

## Part 7 — Update 30 Oct: two weeks on, pages on the side buttons

**Two weeks unattended (`docs/data/battery-2026-10-30.csv`):** `used 305, available 199`:
NVS settled where Part 6 budgeted. 16.6 days on one charge, 4.23 → 3.73 V (~30 mV/day),
battery log unbroken; 10 004 wakes, 2 015 refreshes, 1 timer-arm retry, 0 fallbacks, 0
failed power-offs; 26 daily clock syncs in a row, including the 25 Oct change from BST to
GMT. The battery log keeps ~5 days, so pull it at least that often (16–25 Oct was lost).

Three quick fixes followed, and a fourth after the next log (2 Nov).

**1. Pages on the side buttons.** The power button no longer steps pages. A press only
wakes the board and opens the 30 s window (blue blink) **without refreshing**, so the
buttons answer at once; in the window side button **A** shows the next page and **B** the
previous one. Each step refreshes (red LED, 15–25 s) and restarts the window; presses
during a refresh are dropped. The power button still has to come first: Part 4's reason
stands (A/B are ESP32 pins, and the ESP32 is off in L1). Deep sleep (L2) would let A/B
wake the board directly, but at ~38 mA instead of ~3 mA: a day or two per charge.

| | Before | After |
|---|---|---|
| Power button | next page (refresh, then the window if on USB) | opens the window only, on battery too |
| Side button A / B | nothing | next / previous page, while the window is open |
| Low battery | dashboard only | unchanged: no paging |
| Window on battery | none | 30 s after a power-button press |

`prevPage()` mirrors `nextPage()`; `serialWindow()` takes an optional step callback and
polls G10/G9 for a HIGH → LOW edge (a button held when the window opens doesn't count).
**Result:** power → A (QR) → A (photo) → B (QR) → off 30 s after the last step.

**2. Battery gauge outline.** The yellow fill (21–50 %) had no visible edge on white, so the
level was hard to read. The fill now has a 1 px black outline, in every colour, inside a
1 px white gap. Checked on the glass at `SIMV 3.80`.

**3. The service window while `SIMV` is set.** `SIMV` pretends USB is off, and the window
only opened on USB, so after `SIMV 3.80` a button wake gave no window and `SIMV 0` could
only be sent after a reset. The window now also opens while `SIMV` is set. **Result:**
`SIMV 3.80`, button → window → `SIMV 0` cleared.

> **Lesson:** a test hook that changes what the board believes (here "on USB") must not
> also switch off the way you undo it.

**4. (2 Nov) Plugging in USB opened no window.** The 2½-day log
(`docs/data/battery-2026-11-02.csv`) was fine: the last day fell ~22 mV, the same as on
16 Oct at that voltage, so the paging window costs nothing visible. Pulling it was harder.
The plug-in wake printed `WAKE_SRC 0x02, batt 4.10 V` **without** `(USB)`, did a 26.7 s
light-sleep refresh (the battery path) and powered off without a window.

**Why:** the M5PM1 sets the VIN wake flag **before** `PWR_SRC` bit 0 (5 V in) goes high,
and the firmware read `PWR_SRC` at once. The refresh log had shown it on 11 Oct as well: a
`0x02` wake with `light_sleep=1` (early read: battery) but `usb=1` (read after the
refresh), then a button wake 18 s later, from someone pressing because nothing opened.

**The fix:** on a VIN wake, poll `PWR_SRC` every 20 ms for up to 2 s before deciding.
**Result:** unplug, let it sleep, plug in → `WAKE_SRC 0x02 ... (USB)`, 16.0 s refresh,
service window. Its `USB plug-in: 5 V in seen after N ms` line comes out before the
computer has opened the port, so the real delay is still unmeasured (under 2 s).

> **Lesson:** a wake flag and a status flag from the same chip aren't necessarily updated
> together; when one implies the other, wait for the second instead of trusting a single read.

## Part 8 — Update 22 Nov: a charge log

**The gap:** the battery ran down to 3.75 V (20 Nov) and was charged, and the 22 Nov log
(`docs/data/battery-2026-11-22.csv`) showed the charge as **one point**: 3.75 V at 09:56,
4.23 V at 10:57. The hourly log samples once an hour and has no USB flag. The refresh log
has both, but keeps only ~8 h, so by the next pull it was long overwritten. Nothing on the
board recorded how the charge went.

What could be recovered went into `docs/data/charge-2026-11-20.csv`: the hourly rows plus
four wake lines from that day's serial capture, with their times estimated (the lines carry
no clock) and the USB flag on hourly rows inferred from the voltage, both marked in the file:

| Time (20 Nov) | Battery | |
|---|---|---|
| 09:56 | 3.75 V | on battery |
| ~09:59 | | USB plugged in |
| ~10:02 | 3.90 V | the step is the charger's push (IR rise), not charge |
| ~10:19 | 4.07 V | |
| 10:57 | 4.23 V | |
| 11:57 – 15:58 | 4.19 – 4.21 V | flat: constant-voltage phase, or already done |
| 17:59 → 22:00 | 4.19 → 4.17 V | unplugged (some time after 15:58) |

So: up to ~4.2 V within the hour, then flat for hours. When the charge actually *finished*
can't be seen from this.

**What the board can measure:** only the battery voltage. The charger (IP2315) isn't on
I2C, and the M5PM1 library's `isCharging()` is a stub, so there's no charge current and no
"done" flag. The voltage read while charging is also high by the charger's push, which is
why the readings on USB (4.23 V) and after unplugging differ. The settling after unplugging
is therefore part of the profile too.

**The fix: a charge log** (`clog`, a ring like Part 6's), sampled only around a charge:

| When | Sample |
|---|---|
| on USB, first 2 h after plug-in | every wake (2 min) — the rising part |
| on USB, 2 h – 6 h | every 10 min — the flat part |
| on USB, after 6 h | none, so a day on the charger can't overwrite the start |
| after unplugging, first 30 min | every wake — the relaxation to resting voltage |
| after unplugging, 30 min – 2 h | every 10 min |
| otherwise (on battery) | nothing, and no NVS write |

A plug-in counts as a new charge when USB was absent for over 10 min. Each record is 10
bytes: time, battery mV, temperature, and the raw `PWR_SRC` and `WAKE_SRC` bytes, in
case another `PWR_SRC` bit turns out to mean "charging". A charge takes at most ~108
samples; the ring keeps at least 125 (25 per blob × 6 blobs).

**The storage budget (Part 6's lesson):** each 250-byte blob costs ~10 entries, so the
charge log needs **~64 entries** at full size: from ~312 used / 192 available to about
**376 / 128**, well above the 40-entry warning. Unlike the battery log it survives a fresh
boot, so a flash doesn't lose a charge. It also skips samples while `SIMV` is set (faked
voltage, USB pretended off) or while the clock is invalid.

`LOG` prints the charge log at the end, so `tools/pull_log.py` collects it with no change;
`CHARGELOG` prints it alone.

**Result:** flashed 22 Nov 19:28 on USB; `CHARGELOG` straight after showed the first
sample, `4230 mV, usb 1, PWR_SRC 0x07`. Bits 1 and 2 of `PWR_SRC` are also set; whether
either changes when the charge ends or USB is removed is the first thing to look for. The
first full profile needs a charge from low (~3.7–3.8 V), on any charger — the laptop isn't
needed any more.

**Two things seen on the way:**
- **A charge-only cable** gave no service window at all: the laptop saw something on the
  USB-C port (UCSI errors, `con4: failed to register partner alt modes`) but no USB
  device. A data cable fixed it. No `/dev/ttyACM0` at all, even after a power-button press,
  means suspect the cable before the firmware.
- **`E BOD: Brownout detector was triggered`** follows every power-off **on USB** (also
  in the 20 Nov capture). The PMIC cuts the ESP32's supply while USB keeps feeding it
  partly, so it browns out instead of switching off cleanly. Harmless so far: no reset
  loop, no lost data. It doesn't happen on battery.

> **Lesson:** a log's sampling rate has to match the event it's for. An hourly log is
> right for a two-week discharge and blind to a one-hour charge; rather than log
> everything faster, log fast only around the event (here: USB in, and just after USB out).

## Part 9 — Update 23 Nov: "the buttons don't change pages any more"

**Symptom:** the morning after Part 8's flash, pressing power and then A gave no blue
light and no page change, on USB and on battery. The obvious suspect was the new firmware.

**Evidence first:**
- A 15-min serial capture saw only timer wakes (`WAKE_SRC 0x01`). The window that A/B
  need never opened.
- The kernel log at the moment of the press (12:15) showed the board connecting and
  dropping **four times in 30 s**, with a brownout, and the wake counter never moved: the
  board was switched on, and then off again before it finished booting.
- Nothing in the firmware configures the power button (it belongs to the M5PM1), the
  Part 8 change doesn't touch it, and the libraries are pinned and unchanged since October.

**A diagnostic command, `PMBTN`:** prints the M5PM1's power, wake and IRQ registers
(0x04–0x06, 0x40–0x45), then for 20 s prints every change while you press the button.
Sent in the flash window after uploading it:

```
PM 0x42 = 0x00                                  <- button IRQ status, idle
+ 1463 ms  PM 0x42: 0x00 -> 0x02                <- every press seen ...
+ 1490 ms  PM 0x42: 0x02 -> 0x00                <- ... and cleared by the chip ~25 ms later
   (6 presses in 8 s, all seen)
[12:21:38] E BOD: Brownout detector was triggered   <- power cut WHILE AWAKE
[12:21:40] wake #477 (button/USB, WAKE_SRC 0x0C)    <- the next press: a clean wake + window
```

**Why:** the button works, but **a press while the board is awake makes the M5PM1 cut
the power** (here a hold, or one tap too many). Repeated taps, or holding the button
"to make sure" (which I had suggested), switch the board on and straight off again, so the
blue window never appears. That fits the earlier "first press ignored" cases too.

**Result:** board left alone for 2 min, **one short tap** → blue LED at once → B
(previous: dashboard → dog photo), then A (back to the dashboard). Nothing needed fixing
in the firmware.

**How to use it:** one short tap on power, wait for blue, then A (next) / B (previous).
Don't touch power again while it's blue or refreshing. (A is the button nearer the corner;
B is the second one.)

**Still open:** button wakes now report `WAKE_SRC 0x0C` (bit 3 as well as the power-button
bit 2); on 20 and 22 Nov they were `0x04`. It changes nothing in the firmware's decisions
(it tests bit 2), but what bit 3 means isn't known yet. `PMBTN` stays in the firmware for
the next time the button misbehaves.

> **Lesson:** when "the new firmware broke X", first check that X's input reaches the
> board at all. Here one register watch showed the button worked, and the failure came
> from how it was being pressed. Also: be careful telling someone to "hold it longer" on a
> button whose long press means "off".

## Command cheat-sheet (new in this lesson)

| Command | What |
|---|---|
| `LOC <lat> <lon>` | set the weather location (stored on the board) |
| `WEATHER` | show the stored forecast and when it was fetched |
| `WXNOW` | fetch the weather now |
| `SHOT` | stream the dashboard's frame buffer (use `tools/screenshot.py`) |
| `SHOT ICONS` | the same for a sheet of every weather icon (`tools/screenshot.py icons`) |
| `CHARGELOG` | the charge log alone (Part 8; `LOG` also prints it at the end) |
| `PMBTN` | M5PM1 power/wake/IRQ registers, then 20 s of changes while you press power (Part 9) |

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
| `stages/lesson8_nvs_fix.cpp` | Part 6: packed NVS logs, save guards, storage warning |
| `stages/lesson8_charge_log.cpp` | Part 8: + Part 7's fixes and the charge log; Part 9's `PMBTN` (= `src/main.cpp`) |
| `docs/data/charge-2026-11-*.csv` | Part 8: the charge profiles of 20 Nov (reconstructed) and 22 Nov |

## Glossary

- **API** — a web address that returns data for programs (here JSON) instead of a page.
- **Root certificate** — a certificate your device trusts by itself; a server's certificate
  is accepted if it chains up to one of them.
- **Certificate pinning** — trusting only specific roots, rather than a whole store of them.
- **WMO weather code** — a standard number for the weather (0 clear, 3 overcast, 61 rain...).
- **Frame buffer** — the picture in memory that a refresh sends to the panel.
- **Packed struct** — a struct with no padding bytes, so its layout is exactly as written.

# Lesson 05 — Wi-Fi and NTP: Keeping the Clock Right

**Project:** M5Stack PaperColor (SKU C151) in C++ with PlatformIO
**Goal of this lesson:** Let the battery dashboard correct its own clock over Wi-Fi —
once a day, for a few seconds — **without the Wi-Fi password ever appearing in code,
git, the terminal or this conversation**, and *measure* how far the RTC drifts.

> Audience note: Python / CircuitPython background, learning C++.
> **Status:** complete. Firmware: `stages/lesson5_ntp_sync.cpp`.

---

## Learning objectives

1. Bring up a radio the safe way: first light is a **scan**, not a connection (G32).
2. Keep secrets **out of source code**: provision them at runtime into on-chip storage.
3. Get network time with **SNTP**, and use it to **measure** RTC drift, not just fix it.
4. Fit a radio into a battery design: when to use it, when **not** to (low battery).
5. Notice — again — when a tool is unable to show you the answer.

---

## Design decisions (made before any code)

| Question | Decision | Why |
|---|---|---|
| Where does the Wi-Fi password live? | **Typed in over serial at runtime**, stored in NVS namespace `wifi` | Nothing in any file, so nothing to leak via git; change networks without reflashing. Stored unencrypted on the chip — fine for a home gadget |
| How is it typed? | `tools/wifi_setup.py` asks with Python's **`getpass`**, run in the user's **own** terminal | Not echoed, not in shell history, not in this conversation. The board only ever reports the password's *length* |
| How often to sync? | **Once a day**; immediately if the RTC has lost the time; retry ≥ 1 h after a failure | The RTC drifts ~1 s/day (measured below); the screen shows HH:MM |
| When never? | **In low-battery mode** | Wi-Fi current peaks are the likeliest brownout trigger on a weak cell |
| Where in the wake? | **Before drawing** | The screen then shows the corrected time |
| Radio afterwards | **`WIFI_OFF`** every time | An idle radio silently costs tens of mA (G35) |

The ESP32-S3 is **2.4 GHz only** — the router's 2.4 GHz network is needed.

---

## Part 1 — First light: a scan

A new service-window command `SCAN`:

```cpp
WiFi.mode(WIFI_STA);  WiFi.disconnect();
int n = WiFi.scanNetworks();               // blocks a few seconds
... WiFi.SSID(i), WiFi.RSSI(i), WiFi.channel(i) ...
WiFi.scanDelete();  WiFi.mode(WIFI_OFF);   // radio off again
```

Result: **13 networks**; the home router at **−16 dBm** (RSSI is negative dBm — closer
to 0 is stronger), also visible on a second channel at −57 dBm (a mesh point/extender).

**Cost of `#include <WiFi.h>`:** flash use went **9 % → 18.2 %** — about **590 KB** for
the network stack, WPA and crypto (exactly as G32 warned).

**Tool bug (mine):** the first scan's results never arrived — `tools/service.py` stopped
listening after 2 s of silence, and a scan *is* several seconds of silence. Fixed by
waiting longer. *A tool that stops listening too early reports "nothing".*

## Part 2 — Credentials without the password ever being visible

Service-window commands: `SSID <name>` and `PASS <password>` (each takes the **rest of
the line**, so names with spaces work), `WIFI?` (SSID + password **length** only), and
`WIFI-` (erase). They live in their **own NVS namespace `wifi`**, which the dashboard's
fresh-start `prefs.clear()` (namespace `dash`) does not touch, and which uploads don't
overwrite.

```bash
~/.platformio/penv/bin/python tools/wifi_setup.py    # in your OWN terminal
# Wi-Fi network name (SSID, 2.4 GHz): ...
# Wi-Fi password (not shown):
# Now press the POWER button on the board ...
```

> **Python note:** `getpass.getpass()` reads from the terminal with echo turned off — the
> same thing `sudo` does. The tool also refuses to print any line containing the password.

Board's reply: `wifi: SSID "…", password 14 characters` — the length is how you check
the typing without ever seeing the secret.

## Part 3 — SYNC: measure the drift, then correct it

`ntpSync()` (service command `SYNC`, and later automatic):

1. **Align to the RTC's next second tick** and take a microsecond timestamp
   (`esp_timer_get_time()`), so "what the RTC reads now" can be computed to ~1 ms.
2. **Connect** (15 s timeout). Failures are *named*: `WL_NO_SSID_AVAIL` → "network not
   found", `WL_CONNECT_FAILED` → "wrong password?".
3. **SNTP** (10 s timeout) from `uk.pool.ntp.org` (+ two fallbacks); a callback
   (`sntp_set_time_sync_notification_cb`) says when the clock has been set.
4. **Drift = network time − RTC estimate**, in ms.
5. **Write the RTC on a second boundary** (it stores whole seconds), Wi-Fi off, save a
   history entry in NVS namespace `sync`.

### First result

```
sync: connected to "<home SSID>" in 4759 ms (-57 dBm)
sync: SNTP in 1080 ms; RTC was -4232 ms off (ahead); RTC set to 2026-10-07 11:34:07 UTC
```

The RTC was last set from the PC on **3 October 16:21:06 UTC** (Lesson 03). After
**3 d 19 h 13 min** it was **4.23 s fast**:

| | |
|---|---|
| Drift | 4.232 s / 328,381 s = **≈ 12.9 ppm** (parts per million) |
| Per day | **≈ 1.1 s/day fast** |
| Uncertainty | roughly ±0.03 s (the original PC-based set + this alignment) |

So a **daily** sync is generous; even weekly would stay within ~8 s. A second sync an
hour later found the RTC **−3 ms** off — below the method's resolution: it confirms the
RTC is *written* accurately; drift needs days between syncs to show.

**Radio time per sync:** connect 4.8 s and 8.7 s on two runs (DHCP varies) + SNTP
0.1–1.1 s → **~5–10 s**. At roughly 100 mA that is ~0.2 mAh — well under 1 % of the
~75 mAh/day budget (estimate; not measured on the battery yet).

**Observation:** it joined the **−57/−65 dBm** access point, not the −16 dBm one — the
default connect takes the first match found, not the strongest. Fine here; noted.

## Part 4 — Automatic, daily, and shown on the screen

At the start of every wake (before drawing), the sync runs if all hold: credentials
exist · **due** (≥ 24 h since the last success, or the RTC has lost the time) · **retry
allowed** (≥ 1 h since the last attempt; 10 min if the time is lost) · **not low
battery**. The attempt time is recorded *before* trying, so a hang or failure can't
cause a retry storm.

The status line now reads e.g. `batt 4.18 V - up 0.0 d - synced 3 h ago` (or
`not synced`), and `LOG` includes the sync history:

```
# NTP sync log
n,utc_epoch,local,offset_ms,connect_ms,sntp_ms,rssi_dbm
```

**Testing without waiting a day:** `SYNCDUE` (service command) marks a sync as due:

```
sync marked due: the next wake will sync
sync: connected to "<home SSID>" in 8700 ms (-65 dBm)
sync: SNTP in 120 ms; RTC was -3 ms off (ahead); RTC set to … 11:38:48 UTC
auto-sync OK in 9684 ms
```

The following timer wakes (12:41:48, 12:43:49 in the host's USB log) each lasted
**under a second** — no repeat sync, as intended (a sync wake takes ~10 s).

> **C++ note:** `dumpLog()` is defined *above* `dumpSyncLog()` in the file, but calls it.
> C++ reads top to bottom, so a **forward declaration** — `static void dumpSyncLog();` —
> tells the compiler "this exists, defined later". Python resolves names at run time and
> never needs this.

### Bugs on the way (all in my tools, not the firmware)

| Symptom | Cause | Fix |
|---|---|---|
| Upload: `Invalid head of packet (0x00)` — twice | an **orphaned `service.py`** from the earlier SYNC test still had the port open, stealing esptool's bytes; after each disconnect it went back to waiting and **re-sent its commands on every reconnect** | killed **by PID** (found with `fuser -v /dev/ttyACM0`); the tool now exits for good once it has served one window |
| A background job killed itself | `pkill -f 'service.py …'` matched **its own** command line (the same mistake as in Lesson 04) | stop background jobs by task/PID, never by pattern |
| `last wake 65535 ms` | a long service-window wake (sync + refresh + window > 65 s) saturated the 16-bit counter | harmless — it saturates instead of wrapping |

*Two programs reading one serial port each get a scrambled half of the data.* When a
serial tool misbehaves, `fuser -v /dev/ttyACM0` shows who else has the port.

---

## Open / next

- Drift over **days** (the sync log will show it), and whether daily syncs are visible in
  the battery slope.
- Connect to the **strongest** AP (scan → pick by RSSI → `WiFi.begin(ssid, pass, channel, bssid)`)
  if the weaker one ever proves unreliable.

## Files

| File | What |
|---|---|
| `stages/lesson5_ntp_sync.cpp` | dashboard v3 + Wi-Fi: SCAN, credentials, SYNC, daily auto-sync |
| `tools/wifi_setup.py` | store credentials with `getpass` (run in your own terminal) |
| `tools/service.py` | send service commands (`LOG`, `SCAN`, `SYNC`, `SYNCLOG`, `SYNCDUE`, `WIFI?`, `SIMV`) |

## Command cheat-sheet

```bash
~/.platformio/penv/bin/python tools/wifi_setup.py            # set Wi-Fi (own terminal)
~/.platformio/penv/bin/python tools/service.py "WIFI?" SYNC  # check + sync now
~/.platformio/penv/bin/python tools/service.py SYNCLOG       # drift history
fuser -v /dev/ttyACM0                                        # who has the port?
# ...then press the POWER button for a service window
```

## Glossary

- **SSID** — a Wi-Fi network's name. **RSSI** — received signal strength, in dBm.
- **DHCP** — how the board gets an IP address from the router (most of the connect time).
- **NTP / SNTP** — network time protocol / its simple client form, used by the ESP32.
- **ppm** — parts per million; 1 ppm ≈ 0.086 s per day.
- **NVS namespace** — a named section of the ESP32's key-value flash storage.
- **Forward declaration** — telling the C++ compiler a function exists before its body.

# Draft issue for m5stack/M5GFX (not posted — for the owner to review and post)

**Title:** PaperColor (ED2208): refresh takes 14–39 s, but `_wait_busy()` gives up at 20 s → POWER_OFF sent mid-refresh

**Board:** M5Stack PaperColor (C151), ESP32-S3R8. **Library:** M5GFX 0.2.31 / M5Unified 0.2.24
(PlatformIO, Arduino core 3.3.x). The factory firmware's M5GFX 0.2.21 (`5268353`) has the
same driver code.

## Problem

`Panel_ED2208::_wait_busy(uint32_t timeout = 20000)` returns `false` after 20 s, and
`_turn_on_display()` ignores the return value. On my unit the `DISPLAY_REFRESH` (0x12) busy
period is often **25–39 s**, so the driver sends `POWER_OFF` (0x02) while the panel is still
refreshing (visible on BUSY as a missing power-off phase).

## Measurements (BUSY pin G11 timestamped with an interrupt; driver unchanged)

| | transfer | POWER_ON busy | REFRESH busy | POWER_OFF busy |
|---|---|---|---|---|
| 3 Oct, 8.4–23.5 °C (22 refreshes) | 241 ms | 131 ms | **14,367–14,416 ms** | 152 ms |
| since 7 Oct, stock 20 s timeout | 241 ms | 131 ms | **~26,690 ms** | *(no separate phase)* |
| since 7 Oct, timeout raised to 60 s | 241 ms | 131 ms | **14.2 / 25.0 / 26.7 / 33–39 s** | 152 ms |

- **Until 7 Oct ~13:56** REFRESH was ~14.4 s at every temperature tested (fridge test,
  8.4–23.5 °C inside the case).
- **Since then** it takes one of a few discrete values — 14.2, 25.0, 26.6–26.8, 33–39 s — over
  several hundred logged refreshes, under unchanged firmware, on USB or on battery.
- **Fixed within a power-up:** every refresh between two power-offs of the panel (our
  firmware powers the board down between wakes via the M5PM1) has the same duration, to
  ±5 ms, including across ESP32 restarts that re-send the init sequence; a full power
  cycle can change it.
- **Temperature shifts it now:** a cooling to ~10 °C stepped 14.2 → 25.0 → 26.6 s and back.
  But the same temperature (~21 °C) has also given 14.2, 26.7 and 35.3 s.
- **Content doesn't matter:** all-white, all-black and six solid ink bands measure the same.
- **Forcing a temperature has no effect on a running controller:** `0xE0 = 0x02` then
  `0xE5 = <°C>` (the UC81xx CCSET/TSSET pair), sent after the init sequence and before the
  next refresh, for 0–40 °C → identical REFRESH time. (Not yet tried right after an RST pulse.)
- Minor: the PaperColor panel config sets `cfg.pin_rst = GPIO_NUM_43`, but G43 is the bus
  DC pin and the real reset is G12 (`_pin_reset(GPIO_NUM_12, ...)`). Harmless today because
  the driver calls `init(false)`, but misleading.
- With a microSD card inserted, the 120 KB transfer slows from 241 ms to 639 ms and the BUSY
  pin shows glitch edges (separate issue; the change on 7 Oct coincided with the first boot
  with a card inserted under new firmware — possibly unrelated).

## Suggestions / questions

1. Raise the default timeout (e.g. 60 s) or make it configurable; at minimum don't send
   `POWER_OFF` when `_wait_busy()` timed out.
2. How does the ED2208 choose its refresh program at power-up (temperature bands? a counter
   or other state in the controller's non-volatile memory?) Is a change from "always ~14.4 s"
   to "14–39 s" expected, and can it be read or reset?
3. Does the ED2208 support a forced/external temperature (as UC81xx `0xE0`/`0xE5`), or
   another way to select the program?
4. Is the microSD card's effect on the shared SPI bus (CS G47) known?

Workaround used: a PlatformIO pre-build script that changes the default to 60 s.

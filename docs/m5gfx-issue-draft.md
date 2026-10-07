# Draft issue for m5stack/M5GFX (not posted — for the owner to review and post)

**Title:** PaperColor (ED2208): `_wait_busy()` 20 s timeout is shorter than the panel refresh (~26.7 s) → POWER_OFF sent mid-refresh

**Board:** M5Stack PaperColor (C151), ESP32-S3R8. **Library:** M5GFX 0.2.31 / M5Unified 0.2.24
(PlatformIO, Arduino core 3.3.x). The factory firmware's M5GFX 0.2.21 (`5268353`) has the
same driver code.

## Problem

`Panel_ED2208::_wait_busy(uint32_t timeout = 20000)` returns `false` after 20 s, and
`_turn_on_display()` ignores the return value. On my unit the `DISPLAY_REFRESH` (0x12) busy
period is **~26.7 s**, so the driver sends `POWER_OFF` (0x02) while the panel is still
refreshing.

## Measurements (BUSY pin G11 timestamped with an interrupt; driver unchanged)

| | transfer | POWER_ON busy | REFRESH busy | POWER_OFF busy |
|---|---|---|---|---|
| until 7 Oct (same code, same images) | 241 ms | 131 ms | **14,413 ms** | 152 ms |
| since 7 Oct ~13:56, stock 20 s timeout | 241 ms | 131 ms | **~26,690 ms** | *(no separate phase)* |
| since then, timeout raised to 60 s | 241 ms | 131 ms | **26,694 ms** | 152 ms |

- REFRESH time is independent of content: all-white, all-black and six solid ink bands
  all measure 26,689–26,691 ms.
- It persists across firmware (including code that measured 14.4 s earlier), with or without
  a microSD card, after full power-off cycles, at ~23 °C (earlier ~23.6 °C).
- With a microSD card inserted, the 120 KB transfer slows from 241 ms to 639 ms and the BUSY
  pin shows glitch edges.

## Suggestions / questions

1. Raise the default timeout (e.g. 40–60 s) or make it configurable; at minimum don't send
   `POWER_OFF` when `_wait_busy()` timed out.
2. Is a change from ~14.4 s to ~26.7 s REFRESH expected for this panel (temperature range,
   waveform/LUT selection, a controller setting)? Is there a way to read or reset it?
3. Is the microSD card's effect on the shared SPI bus (CS G47) known?

Workaround used: a PlatformIO pre-build script that changes the default to 60 s.

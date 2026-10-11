# Draft issue for m5stack/M5Unified + M5GFX (not posted — for the owner to review and post)

**Title:** PaperColor: with `clear_display = false` the ED2208 is never reset after power-up → it intermittently runs 26.7–39 s refresh programs (and `_wait_busy()`'s 20 s timeout cuts them short)

**Board:** M5Stack PaperColor (C151), ESP32-S3R8. **Libraries:** M5Unified 0.2.24, M5GFX 0.2.31
(PlatformIO, Arduino core 3.3.x). The factory firmware's M5GFX 0.2.21 (`5268353`) has the
same driver code.

## Summary

1. `M5.begin()` with `cfg.clear_display = false` calls `Display.init_without_reset()`. For the
   PaperColor, M5GFX then switches the panel power on (PM1 GPIO0) and runs
   `_pin_reset(GPIO_NUM_12, use_reset=false)`, which **never pulses RST**. On a board that
   powers down between wakes (M5PM1 power-off + timer wake), every wake powers the ED2208
   up **without a reset**, and it then intermittently runs a long refresh program.
2. `Panel_ED2208::_wait_busy(uint32_t timeout = 20000)` gives up after 20 s and
   `_turn_on_display()` ignores the result, so those long refreshes get `POWER_OFF` (0x02)
   **mid-refresh** (visible on BUSY as a missing power-off phase).

## Measurements (BUSY pin G11 timestamped with an interrupt; timeout raised to 60 s)

REFRESH (0x12) busy time over several days, by temperature inside the case (SHT40):

| Board temp | No reset (414 refreshes) | **RST G12 pulsed after `M5.begin()`** (127 refreshes) |
|---|---|---|
| below 19 °C | 25.0 s ×63, 26.7 s ×19, 33–39 s ×2, 14.2 s ×1 | **25.0 s ×41** |
| 19–20 °C | 25.0 s ×105, 26.7 s ×36, 33–39 s ×4, 14.2 s ×1 | 25.0 s ×17, 14.2 s ×6 |
| 20–21 °C | 14.2 s ×23, 25.0 s ×34, 26.7 s ×4, 33–39 s ×2 | 14.2 s ×12, 25.0 s ×2 |
| above 21 °C | 14.2 s ×89, 25.0 s ×9, 26.7 s ×8, 33–39 s ×14 | **14.2 s ×49** |

- **Without a reset:** 97 of 414 refreshes ran 26.7 s or 33–39 s programs, apparently at
  random; the duration is fixed for as long as the panel stays powered.
- **With a reset:** none; only 14.2 s (above ~20 °C) and 25.0 s (below) — consistent
  temperature compensation, repeatable to ±10 ms.
- Direct A/B on one boot: no reset → 26,672 ms; then six resets → 14,222–14,223 ms each.
- Content doesn't matter (all-white, all-black, six ink bands identical).
- Forcing a temperature (UC81xx-style `0xE0 = 0x02`, `0xE5 = °C`) has no effect.

Workaround used, right after `M5.begin()`:

```cpp
pinMode(12, OUTPUT);
digitalWrite(12, LOW);  delay(20);
digitalWrite(12, HIGH); delay(50);
M5.Display.wakeup();     // Panel_ED2208::setSleep(false) -> _init_sequence()
```

plus a pre-build patch raising the default timeout to 60 s.

## Suggestions

1. Pulse the ED2208's RST after switching its power on even in `init_without_reset()` (the
   reset doesn't change the image on e-paper, so it is safe), or reset in
   `Panel_ED2208::init()`.
2. Raise `_wait_busy()`'s default timeout (e.g. 60 s) or make it configurable; at minimum
   don't send `POWER_OFF` after a timeout.
3. Minor: the PaperColor panel config sets `cfg.pin_rst = GPIO_NUM_43`, but G43 is the bus DC
   pin and the real reset is G12 (`_pin_reset(GPIO_NUM_12, ...)`). Unused today
   (`init(false)`), but misleading.
4. Docs: the PaperColor page lists ESP32 G1 as SD card-detect, but G1 is button C; the
   detect switch is on M5PM1 GPIO1 (with GPIO4 as enable), as the factory firmware uses it.

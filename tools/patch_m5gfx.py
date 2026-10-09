# PlatformIO pre-build script: patch M5GFX's e-paper driver busy wait
# (1) timeout 20 s -> 60 s, (2) an overridable hook in the wait loop.
#
# Why: Panel_ED2208::_wait_busy() gives up after 20 s and its caller ignores the
# result. Since 2026-10-07 this panel's refresh takes ~26.7 s, so the driver sent
# POWER_OFF while the panel was still refreshing (seen as a missing power-off phase
# on the BUSY pin). 60 s gives ample headroom. Lesson 06 has the measurements.
#
# PlatformIO downloads libraries into .pio/libdeps/<env>/ (git-ignored), so the fix
# can't live in the library itself — this script re-applies it before every build.
import os

Import("env")  # noqa: F821  (provided by PlatformIO/SCons)

hdr = os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"), env.subst("$PIOENV"),  # noqa: F821
                   "M5GFX", "src", "lgfx", "v1", "panel", "Panel_ED2208.hpp")
OLD = "bool _wait_busy(uint32_t timeout = 20000);"
NEW = "bool _wait_busy(uint32_t timeout = 60000);  // patched by tools/patch_m5gfx.py (was 20000)"

# 2) Busy-wait hook (Lesson 06): the wait loop's delay(10) becomes a call to a weak
#    function whose default is that same delay(10), so the library behaves as before
#    unless the firmware defines its own (ours light-sleeps until BUSY goes high).
inl = os.path.join(os.path.dirname(hdr), "Panel_ED2208.inl")
WAIT_OLD = """        lgfx::delay(10);
      } while (!gpio_in(_cfg.pin_busy));"""
WAIT_NEW = """        m5gfx_ed2208_busy_wait_hook(_cfg.pin_busy);  // patched by tools/patch_m5gfx.py (was lgfx::delay(10))
      } while (!gpio_in(_cfg.pin_busy));"""
HOOK_ANCHOR = "  bool Panel_ED2208::_wait_busy(uint32_t timeout)"
HOOK_DEF = """  // patched by tools/patch_m5gfx.py: overridable busy-wait step (default = old behaviour)
  extern "C" __attribute__((weak)) void m5gfx_ed2208_busy_wait_hook(int) { lgfx::delay(10); }

"""

if not os.path.exists(hdr):
    print(f"patch_m5gfx: {hdr} not found yet (library not installed?) - skipped")
else:
    src = open(hdr).read()
    if NEW in src:
        print("patch_m5gfx: ED2208 busy timeout already 60 s")
    elif OLD in src:
        open(hdr, "w").write(src.replace(OLD, NEW))
        print("patch_m5gfx: ED2208 busy timeout patched 20 s -> 60 s")
    else:
        # Don't guess: an unexpected line means M5GFX changed — check by hand.
        print("patch_m5gfx: WARNING - expected _wait_busy line not found; NOT patched")

    src = open(inl).read()
    if WAIT_NEW in src:
        print("patch_m5gfx: ED2208 busy-wait hook already in place")
    elif WAIT_OLD in src and src.count(HOOK_ANCHOR) == 1:
        src = src.replace(WAIT_OLD, WAIT_NEW).replace(HOOK_ANCHOR, HOOK_DEF + HOOK_ANCHOR)
        open(inl, "w").write(src)
        print("patch_m5gfx: ED2208 busy-wait hook patched in")
    else:
        print("patch_m5gfx: WARNING - expected busy-wait loop not found; hook NOT patched")

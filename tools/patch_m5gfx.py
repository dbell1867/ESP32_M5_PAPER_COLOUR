# PlatformIO pre-build script: patch M5GFX's e-paper driver busy-wait timeout.
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

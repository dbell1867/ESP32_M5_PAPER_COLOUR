#!/usr/bin/env python3
"""Screenshot the dashboard without refreshing the panel (Lesson 08).

Sends SHOT in a service/flash window; the board draws the dashboard into its frame buffer
and streams it as hex RGB rows. Writes two PNGs into images/screens/ (git-ignored):
  <stamp>-buffer.png  what the firmware drew
  <stamp>-glass.png   after the driver's epd_fastest ink mapping (tools/epd_sim.py) —
                      what the panel would actually show
Never use it on a page showing the Wi-Fi QR code: the image would contain the password.

Usage: ~/.platformio/penv/bin/python tools/screenshot.py [icons]   (then press the power button)
       icons: every weather icon at both sizes instead of the dashboard (SHOT ICONS)
"""
import os, sys, time
import serial

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import epd_sim as sim

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
OUT = os.path.join(ROOT, "images", "screens")
port = "/dev/ttyACM0"
CMD = b"SHOT ICONS\n" if sys.argv[1:] == ["icons"] else b"SHOT\n"
TAG = "-icons" if CMD.startswith(b"SHOT ICONS") else ""


def grab():
    deadline = time.time() + 600
    print("waiting for a service window ...", flush=True)
    while time.time() < deadline:
        try:
            with serial.Serial(port, 115200, timeout=0.5) as s:
                s.write(CMD)              # may be lost if the board is mid-refresh
                sent_after_window = False
                rows, w, h = None, 0, 0
                while time.time() < deadline:
                    l = s.readline().decode(errors="replace").rstrip()
                    if not l:
                        continue
                    if rows is None:
                        if l.startswith("# SHOT BEGIN"):
                            w, h = map(int, l.split()[3:5])
                            rows = []
                        elif "window" in l and not sent_after_window:
                            time.sleep(2)
                            if s.in_waiting == 0:  # nothing started: the first SHOT was lost
                                s.write(CMD)
                            sent_after_window = True
                        else:
                            print(l, flush=True)
                    elif l.startswith("# SHOT END"):
                        return w, h, rows
                    else:
                        if len(l) != w * 6:
                            sys.exit(f"screenshot: row {len(rows)} has {len(l)} chars, expected {w * 6}")
                        rows.append(list(bytes.fromhex(l)))
        except Exception:      # port absent/vanishing (incl. termios.error); SystemExit passes
            time.sleep(0.2)
    sys.exit("screenshot: timed out")


w, h, rows = grab()
if len(rows) != h:
    sys.exit(f"screenshot: got {len(rows)} rows, expected {h}")
os.makedirs(OUT, exist_ok=True)
stamp = time.strftime("%Y%m%d-%H%M%S")
sim.write_png(os.path.join(OUT, f"{stamp}{TAG}-buffer.png"), w, h, rows)

fn, dither = sim.MODES["fastest"]                 # what M5GFX sends in epd_fastest
glass = []
for y in range(h):
    src = [tuple(rows[y][3 * x:3 * x + 3]) for x in range(w)]
    packed = fn(src, y, dither)
    g = []
    for byte in packed:
        g += sim.INK_RGB[byte >> 4] + sim.INK_RGB[byte & 15]
    glass.append(g)
sim.write_png(os.path.join(OUT, f"{stamp}{TAG}-glass.png"), w, h, glass)
print(f"screenshot: {w}x{h} -> images/screens/{stamp}{TAG}-buffer.png and -glass.png")

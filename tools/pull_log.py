#!/usr/bin/env python3
"""Pull the dashboard's RTC-memory logs from a sleeping PaperColor.

Run this, then press any button on the board: the button wake refreshes the
screen (LED red), then opens a 30 s service window (LED blue). This script waits
for the port and the "service window" line, sends LOG, and saves the dump.

Usage: ~/.platformio/penv/bin/python tools/pull_log.py [OUTFILE] [PORT]
"""
import sys, time
import serial

out = sys.argv[1] if len(sys.argv) > 1 else "docs/data/battery-log.csv"
port = sys.argv[2] if len(sys.argv) > 2 else "/dev/ttyACM0"
print("waiting for the board — press any button on it ...", flush=True)
lines, sent = [], False
deadline = time.time() + 600
while time.time() < deadline:
    try:
        with serial.Serial(port, 115200, timeout=0.5) as s:
            while time.time() < deadline:
                l = s.readline().decode(errors="replace").rstrip()
                if not l:
                    continue
                print(l, flush=True)
                if "service window" in l and not sent:
                    s.write(b"LOG\n"); sent = True
                elif sent:
                    lines.append(l)
                    if l.startswith("# wakes"):
                        # The dump may have trailer lines after "# wakes" (e.g. the
                        # timer-arm diagnostics) — keep reading until 2 s of silence.
                        quiet = time.time() + 2
                        while time.time() < quiet:
                            more = s.readline().decode(errors="replace").rstrip()
                            if more:
                                print(more, flush=True); lines.append(more)
                                quiet = time.time() + 2
                        open(out, "w").write("\n".join(lines) + "\n")
                        print(f"saved {len(lines)} lines to {out}")
                        sys.exit(0)
    except Exception:  # incl. termios.error (port vanishing mid-open)
        time.sleep(0.2)
sys.exit("timed out")

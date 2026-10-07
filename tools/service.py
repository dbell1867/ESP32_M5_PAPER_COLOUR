#!/usr/bin/env python3
"""Send commands to the dashboard during a service / flash window.

Waits for the board's port and a "window" line (power-button wake, USB plug-in,
or the 20 s flash window after an upload), sends each command, and prints the
replies until 2 s of silence.

Usage: ~/.platformio/penv/bin/python tools/service.py "SIMV 3.45" [LOG ...]
       (then press the power button, unless a window is already coming)
"""
import sys, time
import serial

cmds = sys.argv[1:] or ["LOG"]
port = "/dev/ttyACM0"
print(f"waiting for a service window to send: {cmds} ...", flush=True)
deadline = time.time() + 600
handled = False   # once a window has been served, a disconnect means "done" — never
                  # go back to waiting (an orphaned copy kept re-sending its commands
                  # on every reconnect and held the port during uploads)
while time.time() < deadline:
    if handled:
        sys.exit(0)
    try:
        with serial.Serial(port, 115200, timeout=0.5) as s:
            # Send right away: the "window" line can be printed before we manage to
            # open the port (a race seen in testing). The board buffers the bytes
            # until its window reads them. Sent again below when the line is seen.
            for c in cmds:
                s.write((c + "\n").encode())
            while time.time() < deadline:
                l = s.readline().decode(errors="replace").rstrip()
                if not l:
                    continue
                print(l, flush=True)
                if "window" in l or "simulated" in l or l.startswith("# battery"):
                    # Commands were already sent when the port opened; sending them
                    # again here made the board run everything TWICE (e.g. two 16 s
                    # photo refreshes). Just wait for the replies.
                    handled = True
                    quiet = time.time() + 40
                    while time.time() < quiet:
                        more = s.readline().decode(errors="replace").rstrip()
                        if more:
                            print(more, flush=True); quiet = time.time() + 40
                    sys.exit(0)
    except Exception:   # SerialException, OSError — and termios.error, which is NEITHER:
        time.sleep(0.2) # raised when the board powers off while the port is being opened

sys.exit("timed out")

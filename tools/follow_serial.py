#!/usr/bin/env python3
"""Follow a board's serial output across deep sleeps.

In deep sleep the ESP32-S3's native USB port disappears and re-enumerates on
every wake, so a normal reader dies at the first disconnect. This one keeps
reopening the port (without resetting the board) and timestamps every line.

Usage: ~/.platformio/penv/bin/python tools/follow_serial.py [PORT] [SECONDS]
"""
import sys, time
import serial

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
end = time.time() + (float(sys.argv[2]) if len(sys.argv) > 2 else 600)
up = False
while time.time() < end:
    try:
        with serial.Serial(port, 115200, timeout=0.5) as s:
            if not up:
                print(f"[{time.strftime('%H:%M:%S')}] -- port up --", flush=True); up = True
            while time.time() < end:
                line = s.readline().decode(errors="replace").rstrip()
                if line:
                    print(f"[{time.strftime('%H:%M:%S')}] {line}", flush=True)
    except (serial.SerialException, OSError):
        if up:
            print(f"[{time.strftime('%H:%M:%S')}] -- port gone (asleep) --", flush=True); up = False
        time.sleep(0.2)

#!/usr/bin/env python3
"""Wait until the board announces a service or flash window, then exit 0.

Why: since Lesson 06 the ESP32 light-sleeps while the panel refreshes, and an
upload started during that refresh fails (esptool: "[Errno 71] Protocol error").
The window opens after the refresh, so upload when this script returns.

Usage: ~/.platformio/penv/bin/python tools/wait_window.py [PORT] [SECONDS]
"""
import sys, time
import serial

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
end = time.time() + (float(sys.argv[2]) if len(sys.argv) > 2 else 900)
while time.time() < end:
    try:
        with serial.Serial(port, 115200, timeout=0.5) as s:
            while time.time() < end:
                line = s.readline().decode(errors="replace").rstrip()
                if line:
                    print(line, flush=True)
                if "service window" in line or "flash window" in line:
                    sys.exit(0)
    except Exception:  # port absent or vanishing (standby, re-enumeration)
        time.sleep(0.2)
print("wait_window: timed out", flush=True)
sys.exit(1)

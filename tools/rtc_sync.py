#!/usr/bin/env python3
"""Set the PaperColor's RTC to the host's UTC time, then measure the offset.

Sends "SET YYYY-MM-DD HH:MM:SS" exactly at a whole-second boundary (the RTC
counts whole seconds), then compares each "epoch N" the board prints with the
host's clock at the moment the line arrives.

Usage: ~/.platformio/penv/bin/python tools/rtc_sync.py [PORT] [--check-only] [SECONDS]
"""
import re, sys, time
from datetime import datetime, timezone
import serial

args = [a for a in sys.argv[1:] if not a.startswith("--")]
port = args[0] if args else "/dev/ttyACM0"
secs = float(args[1]) if len(args) > 1 else 25
check_only = "--check-only" in sys.argv

ser = serial.Serial(port, 115200, timeout=0.2)  # opening does NOT reset the board
if not check_only:
    t = time.time()
    time.sleep(1 - (t % 1))                       # wait for the next whole second
    now = datetime.now(timezone.utc)
    cmd = now.strftime("SET %Y-%m-%d %H:%M:%S\n")
    ser.write(cmd.encode())
    print(f"host sent at {now.isoformat(timespec='milliseconds')}: {cmd.strip()}")

end = time.time() + secs
while time.time() < end:
    line = ser.readline().decode(errors="replace").rstrip()
    if not line:
        continue
    host = time.time()
    m = re.search(r"epoch (\d+(?:\.\d+)?)", line)
    note = f"   [host-board = {host - float(m.group(1)):+.2f} s]" if m else ""
    print(line + note)

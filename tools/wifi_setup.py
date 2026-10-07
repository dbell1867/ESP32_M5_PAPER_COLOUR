#!/usr/bin/env python3
"""Store Wi-Fi credentials on the PaperColor WITHOUT the password appearing anywhere.

Asks for the SSID, then the password with getpass (not echoed, not in your shell
history, not on screen), waits for a service window (press the POWER button), and
sends "SSID <name>", "PASS <password>", "WIFI?". The board only ever reports the
password's length.

Run it in YOUR OWN terminal (not through an assistant):
    ~/.platformio/penv/bin/python tools/wifi_setup.py
"""
import getpass, sys, time
import serial

port = "/dev/ttyACM0"
ssid = input("Wi-Fi network name (SSID, 2.4 GHz): ").strip()
pw = getpass.getpass("Wi-Fi password (not shown): ")
if not ssid or not pw:
    sys.exit("SSID and password are both required")
cmds = [f"SSID {ssid}", f"PASS {pw}", "WIFI?"]
print("Now press the POWER button on the board ...", flush=True)

deadline = time.time() + 300
while time.time() < deadline:
    try:
        with serial.Serial(port, 115200, timeout=0.5) as s:
            for c in cmds:                       # send at once; the board buffers them
                s.write((c + "\n").encode())
                time.sleep(0.2)
            quiet = time.time() + 6
            while time.time() < quiet:
                l = s.readline().decode(errors="replace").rstrip()
                if l and pw not in l:            # never print anything containing it
                    print(l, flush=True)
                    if "password" in l or "wifi:" in l:
                        quiet = time.time() + 2
            sys.exit(0)
    except (serial.SerialException, OSError):
        time.sleep(0.2)
sys.exit("timed out — no service window seen")

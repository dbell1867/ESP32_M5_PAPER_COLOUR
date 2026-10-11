#!/usr/bin/env python3
"""Stage converted photos for the board's flash filesystem (Lesson 07).

Copies images/<name>/<name>.epd4 (made by tools/photo_prep.py) into images/fs/photos/,
the folder `pio run -t uploadfs` turns into the LittleFS image. images/ is git-ignored,
so photos never enter git. Existing crops are copied as they are — nothing is re-made.

Usage: python3 tools/stage_photos.py                # stage every converted photo
       python3 tools/stage_photos.py dog disney     # stage just these
       python3 tools/stage_photos.py --clear dog    # empty the stage first
"""
import os, shutil, sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
IMAGES = os.path.join(ROOT, "images")
STAGE = os.path.join(IMAGES, "fs", "photos")
FRAME = 120000            # 400 x 600 px, 4 bits each
CAPACITY = 0x360000       # the data partition in default_16MB.csv (3.375 MiB)

args = sys.argv[1:]
if "--clear" in args:
    args.remove("--clear")
    shutil.rmtree(STAGE, ignore_errors=True)
os.makedirs(STAGE, exist_ok=True)

names = args or sorted(d for d in os.listdir(IMAGES)
                       if os.path.isfile(os.path.join(IMAGES, d, d + ".epd4")))
for n in names:
    src = os.path.join(IMAGES, n, n + ".epd4")
    if not os.path.isfile(src):
        sys.exit(f"stage: {src} not found - run tools/photo_prep.py on the photo first")
    if os.path.getsize(src) != FRAME:
        sys.exit(f"stage: {src} is {os.path.getsize(src)} bytes, expected {FRAME}")
    shutil.copyfile(src, os.path.join(STAGE, n + ".epd4"))
    print(f"  staged {n}.epd4")

staged = sorted(os.listdir(STAGE))
total = sum(os.path.getsize(os.path.join(STAGE, f)) for f in staged)
print(f"stage: {len(staged)} photo(s), {total} bytes of {CAPACITY} "
      f"(~{CAPACITY // FRAME - 2} fit, allowing for filesystem overhead)")
print("next: wait for a service window, then  pio run -t uploadfs --upload-port /dev/ttyACM0")

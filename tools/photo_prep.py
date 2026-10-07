#!/usr/bin/env python3
"""Prepare a photo for the PaperColor's 400x600 six-ink panel — and preview it.

  1. ImageMagick: auto-rotate (phone EXIF), crop to the panel's 2:3 shape around
     the subject, resize to 400x600, hand back raw RGB bytes (no PIL needed).
  2. Route A preview — what the DRIVER would do (M5GFX epd_quality / fast /
     fastest), using the line-for-line port in tools/epd_sim.py.
  3. Route B — our own Floyd–Steinberg error diffusion into the six exact inks,
     plus the packed 4-bit panel image (120,000 bytes, 2 px/byte) to send later.

Usage:
  python3 tools/photo_prep.py images/dog.jpg [--x 0.5] [--y 0.5] [--zoom 1.0]
    --x/--y   centre of the crop, as a fraction of the (rotated) photo (0..1)
    --zoom    >1 crops tighter around the subject
Writes images/<name>/: source.png, driver_quality.png, driver_fast.png,
driver_fastest.png, fs.png, fs_serp.png, compare.png, <name>.epd4 (route B, ours)
and <name>.jpg (the same 400x600 crop, for route A: the board decodes, driver dithers)
(images/ is git-ignored: personal photos never get committed.)
"""
import argparse, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import epd_sim as sim                          # palette, driver dither ports, PNG writer

W, H = 400, 600
INKS = [(r, g, b, idx) for r, g, b, idx in sim.PALETTE]   # the driver's assumed ink RGB


def load(path, cx, cy, zoom):
    """Crop a 2:3 window centred on (cx, cy), resize to 400x600, return rows of RGB."""
    iw, ih = map(int, subprocess.check_output(
        ["magick", path, "-auto-orient", "-format", "%w %h", "info:"]).split())
    ch = min(ih, iw * 3 / 2) / zoom            # largest 2:3 window that fits, then zoom
    cw = ch * 2 / 3
    x0 = min(max(cx * iw - cw / 2, 0), iw - cw)
    y0 = min(max(cy * ih - ch / 2, 0), ih - ch)
    raw = subprocess.check_output(
        ["magick", path, "-auto-orient", "-crop", f"{cw:.0f}x{ch:.0f}+{x0:.0f}+{y0:.0f}",
         "+repage", "-resize", f"{W}x{H}!", "-depth", "8", "rgb:-"])
    assert len(raw) == W * H * 3, len(raw)
    jpg = os.path.join(os.path.dirname(os.path.abspath(path)),
                       os.path.splitext(os.path.basename(path))[0],
                       os.path.splitext(os.path.basename(path))[0] + ".jpg")
    os.makedirs(os.path.dirname(jpg), exist_ok=True)
    # The same crop as a small baseline JPEG, for route A (the board decodes it and
    # the DRIVER dithers). Baseline, not progressive: embedded decoders prefer it.
    subprocess.check_call(
        ["magick", path, "-auto-orient", "-crop", f"{cw:.0f}x{ch:.0f}+{x0:.0f}+{y0:.0f}",
         "+repage", "-resize", f"{W}x{H}!", "-strip", "-interlace", "none",
         "-sampling-factor", "4:2:0", "-quality", "90", jpg])
    print(f"{os.path.basename(path)}: {iw}x{ih} -> crop {cw:.0f}x{ch:.0f} at "
          f"({x0:.0f},{y0:.0f}) -> {W}x{H}")
    return [[tuple(raw[(y * W + x) * 3:(y * W + x) * 3 + 3]) for x in range(W)]
            for y in range(H)]


def nearest(r, g, b):
    best, bd = None, None
    for pr, pg, pb, idx in INKS:
        d = (r - pr) ** 2 + (g - pg) ** 2 + (b - pb) ** 2
        if bd is None or d < bd:
            bd, best = d, (pr, pg, pb, idx)
    return best


def floyd_steinberg(src, serpentine=False):
    """Error diffusion: round each pixel to the nearest ink and pass the rounding
    error on to unvisited neighbours (7/16 right, 3/16 down-left, 5/16 down,
    1/16 down-right). Returns rows of ink indices."""
    buf = [[list(map(float, p)) for p in row] for row in src]
    out = [[0] * W for _ in range(H)]
    for y in range(H):
        rev = serpentine and (y & 1)           # alternate direction: fewer "worms"
        xs = range(W - 1, -1, -1) if rev else range(W)
        d = -1 if rev else 1
        for x in xs:
            r, g, b = buf[y][x]
            pr, pg, pb, idx = nearest(r, g, b)
            out[y][x] = idx
            er, eg, eb = r - pr, g - pg, b - pb
            for dx, dy, wgt in ((d, 0, 7), (-d, 1, 3), (0, 1, 5), (d, 1, 1)):
                nx, ny = x + dx, y + dy
                if 0 <= nx < W and ny < H:
                    p = buf[ny][nx]
                    p[0] += er * wgt / 16; p[1] += eg * wgt / 16; p[2] += eb * wgt / 16
    return out


def rows_from_packed(fn, src, dither):
    """Run one of the driver's row functions (epd_sim) over the image."""
    rows = []
    for y in range(H):
        packed = fn(src[y], y, dither)
        row = []
        for byte in packed:
            row += sim.INK_RGB[byte >> 4] + sim.INK_RGB[byte & 15]
        rows.append(row)
    return rows


def rows_from_idx(idx):
    return [[c for i in row for c in sim.INK_RGB[i]] for row in idx]


def pack(idx):
    """2 pixels per byte, left pixel in the high nibble — exactly what the panel takes."""
    return bytes((row[x] << 4) | row[x + 1] for row in idx for x in range(0, W, 2))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--x", type=float, default=0.5)
    ap.add_argument("--y", type=float, default=0.5)
    ap.add_argument("--zoom", type=float, default=1.0)
    a = ap.parse_args()
    name = os.path.splitext(os.path.basename(a.image))[0]
    outdir = os.path.join(os.path.dirname(os.path.abspath(a.image)), name)
    os.makedirs(outdir, exist_ok=True)

    src = load(a.image, a.x, a.y, a.zoom)
    imgs = {"source": [[c for p in row for c in p] for row in src]}
    for label, mode in (("driver_quality", "quality"), ("driver_fast", "fast"),
                        ("driver_fastest", "fastest")):
        fn, dither = sim.MODES[mode]
        imgs[label] = rows_from_packed(fn, src, dither)
        print(f"  route A: {label}")
    fs = floyd_steinberg(src)
    fss = floyd_steinberg(src, serpentine=True)
    imgs["fs"], imgs["fs_serp"] = rows_from_idx(fs), rows_from_idx(fss)
    print("  route B: Floyd-Steinberg (plain + serpentine)")

    for k, rows in imgs.items():
        sim.write_png(os.path.join(outdir, f"{k}.png"), W, H, rows)
    order = ["source", "driver_quality", "driver_fast", "fs_serp"]
    gap = [255, 255, 255] * 10
    comp = [sum((imgs[n][y] + (gap if i < len(order) - 1 else []) for i, n in enumerate(order)), [])
            for y in range(H)]
    sim.write_png(os.path.join(outdir, "compare.png"), len(order) * W + 10 * (len(order) - 1), H, comp)
    blob = pack(fss)
    open(os.path.join(outdir, f"{name}.epd4"), "wb").write(blob)
    counts = {}
    for row in fss:
        for i in row:
            counts[i] = counts.get(i, 0) + 1
    names = {0: "black", 1: "white", 2: "yellow", 3: "red", 5: "blue", 6: "green"}
    mix = ", ".join(f"{names[i]} {100 * c // (W * H)}%" for i, c in sorted(counts.items(), key=lambda kv: -kv[1]))
    print(f"  packed panel image: {len(blob)} bytes; ink mix (route B): {mix}")
    print(f"  wrote {outdir}/ (compare.png = source | driver quality | driver fast | ours)")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Simulate M5GFX's Panel_ED2208 colour conversion on the host.

A line-for-line port of the four dither paths in Panel_ED2208.inl (M5GFX
0.2.31), integer maths included, so we can PREDICT what each epd_mode does to
a test image before spending a 16 s refresh on the glass.

Usage:  python3 tools/epd_sim.py [OUTDIR]     (default: sim/)
Writes: source.png, fastest.png, fast.png, text.png, quality.png, compare.png

Caveats: the inks are drawn with the driver's *palette* RGB values, i.e. what
the driver ASSUMES each ink looks like — not a photo of the real panel.
"""
import os
import re
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
INL = os.path.join(HERE, "..", ".pio", "libdeps", "papercolor", "M5GFX", "src",
                   "lgfx", "v1", "panel", "Panel_ED2208.inl")

W, H = 400, 600

# ---- palette: (r, g, b, ink index) — copied from epd_palette[] ------------
PALETTE = [
    (0, 0, 0, 0x0),        # BLACK
    (255, 255, 255, 0x1),  # WHITE
    (255, 243, 56, 0x2),   # YELLOW
    (191, 0, 0, 0x3),      # RED
    (100, 64, 255, 0x5),   # BLUE
    (67, 138, 28, 0x6),    # GREEN
]
INK_RGB = {idx: (r, g, b) for r, g, b, idx in PALETTE}


def load_bayer():
    """Read bayer256[] straight from the driver source, so it can't drift."""
    src = open(INL).read()
    body = re.search(r"bayer256\[256\]\s*=\s*\{(.*?)\};", src, re.S).group(1)
    vals = [int(v) for v in re.findall(r"\d+", body)]
    assert len(vals) == 256, len(vals)
    return vals


BAYER = load_bayer()


# ---- the driver's functions, ported ---------------------------------------
def rgb_to_epd_color(r, g, b):                       # .inl _rgb_to_epd_color
    best, min_dist = 0x1, None
    for pr, pg, pb, idx in PALETTE:
        d = (r - pr) ** 2 + (g - pg) ** 2 + (b - pb) ** 2
        if min_dist is None or d < min_dist:
            min_dist, best = d, idx
    return best


def rgb_to_epd_color_pair(r0, g0, b0, r1, g1, b1):   # .inl _rgb_to_epd_color_pair
    e0 = [(r0 - pr, g0 - pg, b0 - pb) for pr, pg, pb, _ in PALETTE]
    e1 = [(r1 - pr, g1 - pg, b1 - pb) for pr, pg, pb, _ in PALETTE]
    ind0 = [dr * dr + dg * dg + db * db for dr, dg, db in e0]
    ind1 = [dr * dr + dg * dg + db * db for dr, dg, db in e1]
    best, min_dist = 0x11, None
    for i in range(6):
        for j in range(6):
            dr = e0[i][0] + e1[j][0]
            dg = e0[i][1] + e1[j][1]
            db = e0[i][2] + e1[j][2]
            d = dr * dr + dg * dg + db * db + ind0[i] + ind1[j]
            if min_dist is None or d < min_dist:
                min_dist, best = d, (PALETTE[i][3] << 4) | PALETTE[j][3]
    return best


def row_none(src, y, dither):
    out = []
    for x in range(0, W, 2):
        c0 = rgb_to_epd_color(*src[x])
        c1 = rgb_to_epd_color(*src[x + 1]) if x + 1 < W else 0x1
        out.append((c0 << 4) | c1)
    return out


def row_bayer_simple_pair(src, y, dither):
    row = BAYER[(y & 15) << 4:((y & 15) << 4) + 16]
    out = []
    for x in range(0, W, 2):
        bias = ((row[x & 15] * 2 - 255) * dither) >> 8
        r0, g0, b0 = (c + bias for c in src[x])
        r1 = g1 = b1 = 0
        if x + 1 < W:
            bias = ((row[(x + 1) & 15] * 2 - 255) * dither) >> 8
            r1, g1, b1 = (c + bias for c in src[x + 1])
        out.append(rgb_to_epd_color_pair(r0, g0, b0, r1, g1, b1))
    return out


def row_rgb_pair(src, y, dither):
    x_step, y_step = 127 * 29, 129 * 48
    step_value = 129 * 127
    step_diff = step_value // 3
    bias_base = y * y_step % step_value
    out = [0] * (W // 2)
    r0 = g0 = b0 = 0
    for x in range(W + 1):
        r, g, b = src[x] if x < W else (128, 128, 128)
        bias_base -= x_step
        if bias_base < 0:
            bias_base += step_value
        bias_r = bias_base
        bias_g = bias_base - step_diff
        if bias_g < 0:
            bias_g += step_value
        bias_b = bias_g - step_diff
        if bias_b < 0:
            bias_b += step_value
        bias_b = bias_b * 2 - (step_value - 1)
        bias_g = bias_g * 2 - (step_value - 1)
        bias_r = bias_r * 2 - (step_value - 1)
        bias = ((bias_r + bias_g + bias_b) * dither) >> 16
        bias_r = (bias_r * dither) >> 16
        bias_g = (bias_g * dither) >> 16
        bias_b = (bias_b * dither) >> 16
        r += bias + bias_r
        g += bias + bias_g
        b += bias + bias_b
        if x & 1:
            out[x >> 1] = rgb_to_epd_color_pair(r0, g0, b0, r, g, b)
        else:
            r0, g0, b0 = r, g, b
    return out


MODES = {  # mirrors the switch in _exec_transfer
    "fastest": (row_none, 70),
    "fast": (row_bayer_simple_pair, 70),
    "text": (row_rgb_pair, 70),
    "quality": (row_rgb_pair, 140),
}


# ---- the test image (the SAME maths is used on the device in Part 5) -----
def hue(h):  # h in 0..1535 -> fully saturated RGB
    seg, f = h >> 8, h & 255
    return [(255, f, 0), (255 - f, 255, 0), (0, 255, f),
            (0, 255 - f, 255), (f, 0, 255), (255, 0, 255 - f)][seg]


PATCHES = [  # band C: troublesome flat colours
    (128, 128, 128),  # mid grey      -> GREEN in fastest (Part 3)
    (192, 192, 192),  # light grey
    (64, 64, 64),     # dark grey
    (200, 130, 90),   # dusty orange  (the driver comment's example)
    (255, 195, 206),  # pink
    (123, 0, 123),    # purple
    (0, 255, 255),    # cyan
    (0, 0, 123),      # navy
]


def pixel(x, y):
    if y < 100:                                   # A: grey ramp
        v = x * 255 // (W - 1)
        return (v, v, v)
    if y < 200:                                   # B: rainbow
        return hue(x * 1536 // W)
    if y < 300:                                   # C: flat patches
        return PATCHES[x // 50]
    r, g, b = hue(x * 1536 // W)                  # D: hue x lightness
    v = y - 300
    if v < 150:
        k = 150 - v                               # toward white at the top
        return tuple(c + (255 - c) * k // 150 for c in (r, g, b))
    k = v - 150                                   # toward black at the bottom
    return tuple(c * (150 - k) // 150 for c in (r, g, b))


# ---- tiny PNG writer (no PIL needed) --------------------------------------
def write_png(path, w, h, rows):
    raw = b"".join(b"\x00" + bytes(row) for row in rows)
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    open(path, "wb").write(png)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "sim")
    os.makedirs(out, exist_ok=True)
    src = [[pixel(x, y) for x in range(W)] for y in range(H)]
    images = {"source": [[c for p in row for c in p] for row in src]}
    for name, (fn, dither) in MODES.items():
        rows = []
        for y in range(H):
            packed = fn(src[y], y, dither)
            row = []
            for byte in packed:                    # unpack: high nibble = left
                row += INK_RGB[byte >> 4] + INK_RGB[byte & 15]
            rows.append(row)
        images[name] = rows
        print(f"simulated {name}")
    for name, rows in images.items():
        write_png(os.path.join(out, f"{name}.png"), W, H, rows)
    gap = [255, 255, 255] * 10                     # side-by-side, 10 px gaps
    order = ["source", "fastest", "fast", "text", "quality"]
    comp = [sum((images[n][y] + (gap if i < 4 else []) for i, n in enumerate(order)), [])
            for y in range(H)]
    write_png(os.path.join(out, "compare.png"), 5 * W + 40, H, comp)
    print(f"wrote PNGs to {os.path.abspath(out)}")


if __name__ == "__main__":
    main()

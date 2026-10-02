#!/usr/bin/env python3
"""Render console OLED scenarios to one PNG sheet, lit amber like the panel (build.sh first).

    python3 tools/console_preview/preview.py out.png [theme] [scenario ...]
    theme: 0 Classic (default), 1 Fallout, 2 Modern
"""
import os
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
RENDER = os.path.join(HERE, "build", "render")
ALL = ["media", "radio", "hints", "trip", "engine", "clock", "menu", "menu-radio", "menu-phone", "menu-display",
       "edit", "source", "pair", "alert", "call-in", "call-active", "volume", "toast", "nophone", "reverse", "waiting", "boot"]
AMBER = (255, 140, 26)
K = 3  # pixels per OLED pixel


def lit(pgm):
    g = Image.open(pgm).convert("L")
    rgb = Image.new("RGB", g.size)
    px = [(0, 0, 0)] * 0
    lut = []
    for v in range(256):
        f = (v / 255) ** 1.15
        lut.append((int(AMBER[0] * f + (14 if v else 10)), int(AMBER[1] * f + (5 if v else 5)), int(AMBER[2] * f + 1)))
    rgb.putdata([lut[v] for v in g.getdata()])
    big = rgb.resize((256 * K, 64 * K), Image.NEAREST)
    glow = big.filter(ImageFilter.GaussianBlur(K * 0.8))
    out = Image.blend(big, glow, 0.2)
    d = ImageDraw.Draw(out)
    for x in range(K, 256 * K, K):
        d.line([(x - 1, 0), (x - 1, 64 * K)], fill=(0, 0, 0))
    for y in range(K, 64 * K, K):
        d.line([(0, y - 1), (256 * K, y - 1)], fill=(0, 0, 0))
    return out


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "console.png"
    theme = sys.argv[2] if len(sys.argv) > 2 else "0"
    scen = sys.argv[3:] or ALL
    tiles = []
    with tempfile.TemporaryDirectory() as tmp:
        for s in scen:
            p = os.path.join(tmp, s + ".pgm")
            subprocess.run([RENDER, s, theme, p], check=True)
            tiles.append((s, lit(p)))
    cols = 2
    w, h = 256 * K, 64 * K
    rows = (len(tiles) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * (w + 24) + 24, rows * (h + 44) + 16), (13, 12, 10))
    d = ImageDraw.Draw(sheet)
    for i, (name, im) in enumerate(tiles):
        x = 24 + (i % cols) * (w + 24)
        y = 16 + (i // cols) * (h + 44)
        d.text((x, y), name, fill=(167, 157, 136))
        sheet.paste(im, (x, y + 18))
    sheet.save(out)
    print(out)


if __name__ == "__main__":
    main()

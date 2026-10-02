#!/usr/bin/env python3
"""Render gauge faces on the host and tile them into one PNG (needs Pillow).

    tools/gauge_preview/build.sh
    python3 tools/gauge_preview/preview.py out.png 0,1,0 2,1,0 1,1,0

Each spec is theme,face,variant[,nodata[,phone]]: theme 0 Classic / 1 Fallout / 2 Modern; face 0 Speedo, 1 Tach,
2 Clock, 3 Media, 4 Econ, 5 Right; variant = right display Classic layout (0 four gauges, 1 fuel + lamps);
phone = the call card over the face, as `phone demo` on the consoles (incoming, active, waiting, held, pair;
modifiers with +, e.g. active+muted+onphone); menu = the menu open on a page over the face (0 home, 2 … 6 phone),
optionally with knob keys after a colon, separated by ; (r<n> rotate, p press, b back), e.g. 6:r2;p.

    python3 tools/gauge_preview/preview.py phone.png 0,1,0,0,idle,6 2,1,0,0,pair,6 1,1,0,0,discover,6
"""
import os
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
RENDER = os.path.join(HERE, "build", "render")


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    out, specs = sys.argv[1], sys.argv[2:]
    tiles = []
    with tempfile.TemporaryDirectory() as tmp:
        for i, spec in enumerate(specs):
            p = spec.split(",")
            theme, face, variant = p[0], p[1], p[2] if len(p) > 2 else "0"
            nodata = p[3] if len(p) > 3 else "0"
            env = dict(os.environ)
            if len(p) > 4 and p[4]:
                env["PHONE"] = p[4].replace("+", " ")
            if len(p) > 5 and p[5]:
                env["MENU"] = p[5].replace(";", ",")
            ppm = os.path.join(tmp, f"{i}.ppm")
            r = subprocess.run([RENDER, ppm, theme, face, variant, "800", "800", nodata, "2"],
                               capture_output=True, text=True, check=True, env=env)
            sys.stderr.write(f"{spec}: {' | '.join(r.stderr.strip().splitlines())}\n")
            im = Image.open(ppm).convert("RGB")
            mask = Image.new("L", im.size, 0)
            ImageDraw.Draw(mask).ellipse((0, 0, im.size[0] - 1, im.size[1] - 1), fill=255)
            tile = Image.new("RGB", im.size, (60, 60, 60))
            tile.paste(im, (0, 0), mask)
            tiles.append(tile)
    cols = min(len(tiles), 3)
    rows = (len(tiles) + cols - 1) // cols
    size = tiles[0].size[0]
    sheet = Image.new("RGB", (cols * size, rows * size), (60, 60, 60))
    for i, t in enumerate(tiles):
        sheet.paste(t, ((i % cols) * size, (i // cols) * size))
    sheet.save(out)


if __name__ == "__main__":
    main()

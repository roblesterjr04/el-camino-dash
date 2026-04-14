# Fallout theme bitmaps (optional)

Vehicle Fallout skins are drawn **procedurally** in firmware today (`center_fallout.cpp`, `speedo_fallout.cpp`, `tsps_fallout.cpp`). Drop **PNG** sources here when you want RGB565 sprites (car silhouette, icons, custom fonts-as-bitmaps).

## Export

From the repo root (requires Pillow):

```bash
python3 -m venv .venv-assets && .venv-assets/bin/pip install pillow
.venv-assets/bin/python scripts/export_fallout_rgb565_assets.py \
  firmware/esp32_p4_center/src --edge 480
```

Repeat with `--out-dir` for speedo/TSPS if you split assets per gauge. The script writes `fallout_assets_gen.h` with `FALLOUT_ASSETS_HAVE` and one array per PNG.

## Flash budget

Large full-screen assets can exceed app partition headroom. Prefer small sprites, reuse glyphs, or ship heavy art on SD and load at boot.

# Scripts

Asset export for the firmware.

## export_bootscreen_rgb565.py

Converts the boot splash image into C headers of RGB565 pixels, one per panel size. The round displays copy the
matching image straight into the panel's framebuffer at power-up, before LVGL starts
(`firmware/esp32_p4_common/include/boot_splash.h`, called from `center_main.cpp` and `side_main.cpp` in
`firmware/esp32_p4_display/src/`).

**When to use it:** after changing the splash image. The generated headers are committed, so a normal firmware build
does not need this script.

**Needs:** Python 3 and Pillow (`pip install pillow`).

```
python3 scripts/export_bootscreen_rgb565.py [--src PNG] [--out-dir DIR]
```

| Argument | Default | Meaning |
| --- | --- | --- |
| `--src` | `assets/bootscreen/boot3.png` | Source image (any image Pillow opens; transparency is composited on black) |
| `--out-dir` | `firmware/esp32_p4_common/include` | Where the headers go (created if missing) |

Both defaults are resolved from the repository root, so the script runs from any directory.

```bash
python3 scripts/export_bootscreen_rgb565.py
python3 scripts/export_bootscreen_rgb565.py --src my_splash.png --out-dir /tmp/splash
```

### Output

Three headers, each the source scaled to fit (aspect ratio kept, Lanczos resampling) and centred on black:

| File | Size | Panel |
| --- | --- | --- |
| `bootscreen_rgb565_800_gen.h` | 800 × 800 | Waveshare 3.4" (SH8601) |
| `bootscreen_rgb565_720_gen.h` | 720 × 720 | Waveshare 4" (JD9365) and the 720 × 720 EV board panel |
| `bootscreen_rgb565_1024x600_gen.h` | 1024 × 600 | Espressif P4 EV board's stock 7" panel |

Each defines `ELC_BOOT_SPLASH_W`, `ELC_BOOT_SPLASH_H` and `static const uint16_t elc_boot_splash_rgb565[]` (one 16-bit
word per pixel, row by row). The firmware picks the header from the panel's build flags. The files are overwritten
on every run and say "do not edit" at the top.

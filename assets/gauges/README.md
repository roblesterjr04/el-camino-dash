# Gauge source art

Source PNGs for the display firmware. The `scripts/export_*_rgb565_assets.py` scripts convert them into the `*_gen.h` headers compiled into each app (see the comments at the top of each `platformio.ini` for the exact command).

| Folder | Used by |
| --- | --- |
| `classic_1/` | center: tach, clock, MPG, media (searched first) |
| `classic_1_rect/` | TSPS classic_1 |
| `classic_2_rect/` | speedo; TSPS classic_2 (`full_gauges.png` + mask); center fallback |
| `fallout/` | optional Fallout sprites (the Fallout screens are procedural by default) |
| `../bootscreen/boot3.png` | boot splash on all three displays |

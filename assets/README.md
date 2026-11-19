# Assets

Images used by the firmware or kept as design reference. The gauge faces themselves are not bitmaps: they are drawn in
code by `firmware/esp32_p4_ui/src/gauges/`.

| Path | Used by the firmware | What |
| --- | --- | --- |
| [`bootscreen/boot3.png`](bootscreen/) | Yes, through generated headers | The boot splash on the round displays |
| [`bootscreen/boot.png`, `boot2.png`, `boot2.webp`](bootscreen/) | No | Earlier boot splash images |
| [`gauges/`](gauges/) | No | The original Classic dial art, kept as the design reference for the vector faces. See [`gauges/README.md`](gauges/README.md) |

## bootscreen/

`boot3.png` (1200 × 1200, with transparency) is the splash each round display shows at power-up, before LVGL starts.
The firmware does not read the PNG: [`scripts/export_bootscreen_rgb565.py`](../scripts/README.md) converts it into
`firmware/esp32_p4_common/include/bootscreen_rgb565_*_gen.h` (800 × 800, 720 × 720 and 1024 × 600), and those headers
are compiled in. After changing the image, run the script and commit the headers with it.

`boot.png` (308 × 308), `boot2.png` (512 × 510) and `boot2.webp` are earlier splash images. Nothing in the firmware or
the scripts refers to them.

## gauges/

The Classic theme's original dial art as PNG layers (dial bases, masks, needles, lamp icons), in three folders:
`classic_1/`, `classic_1_rect/` and `classic_2_rect/`. The firmware no longer uses them; the vector faces in
`faces_classic.cpp` follow their layouts. Which folder is the reference for which face is in
[`gauges/README.md`](gauges/README.md).

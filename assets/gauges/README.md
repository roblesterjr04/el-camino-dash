# Gauge reference art

The original Classic dial art (PNG). The firmware no longer uses these files: every gauge face is drawn in vector
form by `firmware/esp32_p4_ui/src/gauges/`, and the Classic layouts (scales, numerals, chrome, spun-aluminium caps)
follow this art. Keep them as the design reference.

| Folder | Reference for |
| --- | --- |
| `classic_1/` | center tach, clock, MPG, media; right display classic_1 (fuel + lamps) |
| `classic_1_rect/` | right display classic_1 |
| `classic_2_rect/` | speedometer; right display classic_2 (four gauges) |

The boot splash (`../bootscreen/boot3.png`) is still a bitmap; `scripts/export_bootscreen_rgb565.py` converts it.

# LVGL fonts (generated)

Generated with `lv_font_conv` 1.5.2 (`npx lv_font_conv@1.5.2`), uncompressed, ASCII 0x20–0x7E plus ° · — …
Glyph bitmaps are 4 bpp (VT323: 2 bpp).

| File | Source font | Size | Use |
| --- | --- | --- | --- |
| `elc_bc_58`, `elc_bc_46` | Barlow Condensed SemiBold | 58, 46 | Classic menu labels, titles |
| `elc_b_28`, `elc_b_22` | Barlow Medium | 28, 22 | Classic body, captions |
| `elc_vt_72` … `elc_vt_28` | VT323 | 72, 56, 36, 28 | Fallout |
| `elc_sa_56`, `elc_sa_46` | Saira Semi Condensed SemiBold | 56, 46 | Modern labels, titles |
| `elc_sm_28`, `elc_sm_22` | Saira Semi Condensed Medium | 28, 22 | Modern body, captions |
| `elc_icons_56`, `elc_icons_34` | Material Icons (Regular) | 56, 34 | Menu icons (code points in `center_menu.cpp`) |

Barlow, Saira and VT323 are SIL Open Font License 1.1 (github.com/google/fonts); Material Icons is Apache 2.0
(github.com/google/material-design-icons).

Regenerate one (example):

```bash
npx -y lv_font_conv@1.5.2 --no-compress --no-prefilter --bpp 4 --size 58 \
  --font BarlowCondensed-SemiBold.ttf -r 0x20-0x7E,0xB0,0xB7,0x2014,0x2026 \
  --format lvgl --lv-include lvgl.h -o elc_bc_58.c
```

# LVGL fonts (generated)

Shared by all three displays (declared in `include/elc_fonts.h`). Generated with `lv_font_conv` 1.5.2
(`npx lv_font_conv@1.5.2`), uncompressed, ASCII 0x20–0x7E plus ° · — …; a `d` suffix means digits and punctuation
only (`0x20,0x25,0x27,0x2C-0x3A,0xB0,0x2014`: space % ' , - . / 0–9 : ° —). Glyph bitmaps are 4 bpp (VT323: 2 bpp).

| File | Source font | Size | Use |
| --- | --- | --- | --- |
| `elc_bc_64d` | Barlow Condensed SemiBold | 64 | Classic dial numerals |
| `elc_bc_58`, `elc_bc_46`, `elc_bc_34` | Barlow Condensed SemiBold | 58, 46, 34 | Classic numerals, labels, menu titles |
| `elc_b_28`, `elc_b_22` | Barlow Medium | 28, 22 | Classic body, captions |
| `elc_vt_220d`, `elc_vt_120d` | VT323 | 220, 120 | Fallout big readouts |
| `elc_vt_72` … `elc_vt_28` | VT323 | 72, 56, 36, 28 | Fallout |
| `elc_sa_180d`, `elc_sa_136d` | Saira Semi Condensed SemiBold | 180, 136 | Modern big readouts |
| `elc_sa_56`, `elc_sa_46`, `elc_sa_38` | Saira Semi Condensed SemiBold | 56, 46, 38 | Modern values, titles |
| `elc_sm_28`, `elc_sm_22`, `elc_sm_16` | Saira Semi Condensed Medium | 28, 22, 16 | Modern body, captions |
| `elc_icons_56`, `elc_icons_34` | Material Icons (Regular) | 56, 34 | Menu and media icons (code points in `center_menu.cpp`) |

Barlow, Saira and VT323 are SIL Open Font License 1.1 (github.com/google/fonts); Material Icons is Apache 2.0
(github.com/google/material-design-icons).

Regenerate one (example):

```bash
npx -y lv_font_conv@1.5.2 --no-compress --no-prefilter --bpp 4 --size 58 \
  --font BarlowCondensed-SemiBold.ttf -r 0x20-0x7E,0xB0,0xB7,0x2014,0x2026 \
  --format lvgl --lv-include lvgl.h -o elc_bc_58.c
```

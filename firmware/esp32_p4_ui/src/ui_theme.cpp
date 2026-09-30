#include "ui_theme.h"

#include <elc_fonts.h>

UiTheme ui_theme(uint8_t t) {
  UiTheme th{};
  if (t == 1) {
    th = {lv_color_hex(0x010a04), lv_color_hex(0x5dff8f), lv_color_hex(0x2fae5c), lv_color_hex(0x17602f),
          lv_color_hex(0xb8ffcc), lv_color_hex(0x021008), lv_color_hex(0x06200f), lv_color_hex(0x1a6a35),
          lv_color_hex(0xb8ffcc), lv_color_hex(0xffb347), &elc_vt_72, &elc_vt_56, &elc_vt_36, &elc_vt_28};
  } else if (t == 2) {
    th = {lv_color_hex(0x060708), lv_color_hex(0xf3f5f6), lv_color_hex(0x9aa4aa), lv_color_hex(0x5d666c),
          lv_color_hex(0xff7a2f), lv_color_hex(0x0b0c0d), lv_color_hex(0x171b1e), lv_color_hex(0x2b3136),
          lv_color_hex(0x45d483), lv_color_hex(0xff3b30), &elc_sa_56, &elc_sa_46, &elc_sm_28, &elc_sm_22};
  } else {
    th = {lv_color_hex(0x0c0b09), lv_color_hex(0xefe7d5), lv_color_hex(0xa79d88), lv_color_hex(0x6f6758),
          lv_color_hex(0xe8a33d), lv_color_hex(0x1e1405), lv_color_hex(0x1d1a15), lv_color_hex(0x3d372e),
          lv_color_hex(0x7fcf8a), lv_color_hex(0xff6a55), &elc_bc_58, &elc_bc_46, &elc_b_28, &elc_b_22};
  }
  return th;
}

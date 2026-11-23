#include "fw_overlay.h"

#include <elc_fonts.h>
#include <elc_fw.h>
#include <p4_screen.h>
#include <stdio.h>

namespace {

const uint32_t kFailShowMs = 8000;

lv_obj_t *s_panel = nullptr, *s_title = nullptr, *s_pct = nullptr, *s_bar = nullptr, *s_detail = nullptr;
ElcFwTarget::Phase s_last = ElcFwTarget::Phase::Idle;
uint32_t s_fail_ms = 0;
int s_shown_pct = -1;
bool s_visible = false;

lv_obj_t *text(const lv_font_t *f, lv_color_t c, int dy) {
  lv_obj_t *l = lv_label_create(s_panel);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, c, 0);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(l, LV_ALIGN_CENTER, 0, dy);
  return l;
}

void build() {
  lv_obj_t *parent = p4_screen_ui_parent();
  const int w = lv_obj_get_width(parent), h = lv_obj_get_height(parent);
  const int size = w < h ? w : h;
  s_panel = lv_obj_create(parent);
  lv_obj_remove_style_all(s_panel);
  lv_obj_set_size(s_panel, w, h);
  lv_obj_set_style_bg_color(s_panel, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
  lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_panel, LV_OBJ_FLAG_CLICKABLE);  // swallow touches meant for the menu underneath
  lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
  const lv_color_t cream = lv_color_hex(0xF2E6C8), grey = lv_color_hex(0x9A948A);
  s_title = text(&elc_b_28, cream, -size / 7);
  s_pct = text(&elc_bc_58, cream, 0);
  s_bar = lv_bar_create(s_panel);
  lv_obj_set_size(s_bar, size / 2, size / 40 < 8 ? 8 : size / 40);
  lv_obj_align(s_bar, LV_ALIGN_CENTER, 0, size / 11);
  lv_bar_set_range(s_bar, 0, 1000);
  lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x2A2824), LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_bar, lv_color_hex(0xE8A33A), LV_PART_INDICATOR);
  s_detail = text(&elc_b_22, grey, size / 6);
  lv_label_set_long_mode(s_detail, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(s_detail, size * 6 / 10);
}

void show(bool on) {
  if (!s_panel || on == s_visible) return;
  s_visible = on;
  if (on) {
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_panel);
  } else {
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
  }
}

}  // namespace

bool fw_overlay_poll(const ElcFwTarget *t, uint32_t now_ms) {
  if (!t) return false;
  const ElcFwTarget::Phase ph = t->phase();
  const bool changed = ph != s_last;
  if (changed && ph == ElcFwTarget::Phase::Failed) s_fail_ms = now_ms ? now_ms : 1;
  const bool failing = ph == ElcFwTarget::Phase::Failed && s_fail_ms && now_ms - s_fail_ms < kFailShowMs;
  const bool on = t->active() || ph == ElcFwTarget::Phase::Done || failing;
  if (!on) {
    show(false);
    s_last = ph;
    return false;
  }
  if (!s_panel) build();
  show(true);
  const int pct = t->progress() / 10;
  if (changed) {
    s_shown_pct = -1;
    switch (ph) {
      case ElcFwTarget::Phase::Receiving: lv_label_set_text(s_title, "Updating firmware"); break;
      case ElcFwTarget::Phase::Writing:
        lv_label_set_text(s_title, "Writing firmware");
        lv_label_set_text(s_detail, "Keep the ignition on");
        break;
      case ElcFwTarget::Phase::Done:
        lv_label_set_text(s_title, "Firmware updated");
        lv_label_set_text(s_pct, "");
        lv_label_set_text(s_detail, "Restarting");
        lv_bar_set_value(s_bar, 1000, LV_ANIM_OFF);
        break;
      default:
        lv_label_set_text(s_title, "Update failed");
        lv_label_set_text(s_pct, "");
        lv_label_set_text(s_detail, "The old firmware stays. Try again from the console.");
        break;
    }
  }
  if (ph == ElcFwTarget::Phase::Receiving && pct != s_shown_pct) {
    s_shown_pct = pct;
    char b[48];
    snprintf(b, sizeof(b), "%d%%", pct);
    lv_label_set_text(s_pct, b);
    lv_bar_set_value(s_bar, t->progress(), LV_ANIM_OFF);
    const ElcFwHeader &h = t->header();
    if (h.variant[0]) {
      snprintf(b, sizeof(b), "%s, %lu KB", h.variant, (unsigned long)(h.image_size / 1024));
      lv_label_set_text(s_detail, b);
    } else {
      lv_label_set_text(s_detail, "Keep the ignition on");
    }
  }
  s_last = ph;
  return true;
}

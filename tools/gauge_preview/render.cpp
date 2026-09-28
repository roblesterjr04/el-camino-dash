// Host renderer: draws gauge faces with the firmware's LVGL code and writes PPM images.
//   render <out.ppm> <theme 0..2> <face 0..5> <variant> [W H] [nodata] [steps]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lvgl.h>
#include <elc_gauges.h>

void host_set_ms(uint32_t ms);
static uint16_t *g_fb;
static int W = 800, H = 800;
static long g_flushed = 0;

static void flush_cb(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *p) {
  const int w = lv_area_get_width(a);
  g_flushed += (long)w * lv_area_get_height(a);
  for (int y = a->y1; y <= a->y2; y++) memcpy(g_fb + (size_t)y * W + a->x1, p + (size_t)(y - a->y1) * w, (size_t)w * 2);
  lv_disp_flush_ready(d);
}

static GaugeData sample(int step) {
  GaugeData d;
  d.fresh = true;
  d.mph = 47 + step * 3;
  d.rpm = 2850 + step * 150;
  d.fuel_pct = 64;
  d.coolant_f = 195;
  d.oil_psi = 38;
  d.amp = 20;
  d.charging = 1;
  d.odo_valid = true;
  d.odo_mi = 96671.7f;
  d.trip_mi = 12.3f;
  d.gear = 3;
  d.econ_valid = true;
  d.mpg_avg = 17.6f;
  d.mpg_inst = 21;
  d.dte_mi = 245;
  d.time_valid = true;
  d.hour = 10;
  d.minute = 8;
  d.second = 32;
  d.date_text = "Mon, Sep 28";
  d.place_text = "Liberty Street";
  d.media_connected = true;
  d.media_title = "billy";
  d.media_artist = "Stephen Wilson Jr.";
  d.media_elapsed = 83 + step;
  d.media_duration = 215;
  // A synthetic 200×200 cover (sunset gradient, sun, stripes) in a 208-px stride like the P4 decoder's.
  static uint16_t art[208 * 200];
  static bool made = false;
  if (!made) {
    made = true;
    for (int y = 0; y < 200; y++) {
      for (int x = 0; x < 208; x++) {
        float r = 40 + 200 * y / 200.0f, g = 30 + 90 * (1 - y / 200.0f), b = 120 - 80 * y / 200.0f;
        const float dx = x - 100, dy = y - 120;
        if (dx * dx + dy * dy < 45 * 45) { r = 255; g = 200; b = 80; }
        if (y > 150 && ((y / 6) % 2 == 0)) { r *= 0.4f; g *= 0.4f; b *= 0.6f; }
        art[y * 208 + x] = (uint16_t)((((int)r >> 3) << 11) | (((int)g >> 2) << 5) | ((int)b >> 3));
      }
    }
  }
  if (getenv("ART")) {
    d.art_px = art;
    d.art_w = 200;
    d.art_h = 200;
    d.art_stride = 208;
    d.art_id = 1;
  }
  return d;
}

int main(int argc, char **argv) {
  if (argc < 5) { fprintf(stderr, "usage\n"); return 1; }
  if (argc >= 7) { W = atoi(argv[5]); H = atoi(argv[6]); }
  const bool nodata = argc >= 8 && atoi(argv[7]);
  const int steps = argc >= 9 ? atoi(argv[8]) : 0;
  lv_init();
  g_fb = (uint16_t *)calloc((size_t)W * H, 2);
  static lv_disp_draw_buf_t db;
  static lv_color_t *strip = (lv_color_t *)malloc((size_t)W * 48 * 2);
  lv_disp_draw_buf_init(&db, strip, nullptr, (uint32_t)W * 48);
  static lv_disp_drv_t dd;
  lv_disp_drv_init(&dd);
  dd.hor_res = W; dd.ver_res = H; dd.flush_cb = flush_cb; dd.draw_buf = &db; 
  lv_disp_drv_register(&dd);
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  const int size = W < H ? W : H;
  gauges_begin(scr, (W - size) / 2, (H - size) / 2, size);
  gauges_show((GaugeTheme)atoi(argv[2]), (GaugeFace)atoi(argv[3]), (uint8_t)atoi(argv[4]));
  uint32_t ms = 1000;
  GaugeData d = sample(0);
  d.fresh = !nodata;
  host_set_ms(ms);
  gauges_update(d, ms);
  lv_refr_now(nullptr);
  fprintf(stderr, "paint %u ms, first frame %ld px\n", gauges_last_paint_ms(), g_flushed);
  for (int s = 1; s <= steps; s++) {
    // settle smoothing, then move one step
    d = sample(s);
    d.fresh = !nodata;
    for (int k = 0; k < 40; k++) { ms += 16; host_set_ms(ms); gauges_update(d, ms); g_flushed = 0; lv_refr_now(nullptr); }
  }
  if (steps) {
    d = sample(steps + 1);
    d.fresh = !nodata;
    ms += 16; host_set_ms(ms); g_flushed = 0; gauges_update(d, ms); lv_refr_now(nullptr);
    fprintf(stderr, "one step later: %ld px redrawn (%.1f%% of screen)\n", g_flushed, 100.0 * g_flushed / (W * H));
    for (int k = 0; k < 60; k++) { ms += 16; host_set_ms(ms); gauges_update(d, ms); lv_refr_now(nullptr); }
  }
  FILE *f = fopen(argv[1], "wb");
  fprintf(f, "P6\n%d %d\n255\n", W, H);
  for (int i = 0; i < W * H; i++) {
    uint16_t c = g_fb[i];
    unsigned char rgb[3] = {(unsigned char)(((c >> 11) & 31) * 255 / 31), (unsigned char)(((c >> 5) & 63) * 255 / 63), (unsigned char)((c & 31) * 255 / 31)};
    fwrite(rgb, 1, 3, f);
  }
  fclose(f);
  return 0;
}

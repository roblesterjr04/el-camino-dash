#include "center_camera.h"

#include <Arduino.h>
#include <cstring>

#include <cluster_i2c.h>
#include <elc_fonts.h>
#include <esp_cache.h>
#include <esp_cam_ctlr.h>
#include <esp_cam_ctlr_dvp.h>
#include <esp_heap_caps.h>
#include <esp_rom_gpio.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <lvgl.h>
#include <p4_screen.h>
#include <soc/gpio_pins.h>
#include <soc/gpio_sig_map.h>

namespace {

// ---------------------------------------------------------------- wiring (DigiDash J13 → center header)
constexpr gpio_num_t kData[8] = {GPIO_NUM_20, GPIO_NUM_21, GPIO_NUM_22, GPIO_NUM_29,
                                 GPIO_NUM_30, GPIO_NUM_31, GPIO_NUM_32, GPIO_NUM_28};
constexpr gpio_num_t kPclk = GPIO_NUM_49;
constexpr gpio_num_t kVsync = GPIO_NUM_50;
constexpr gpio_num_t kHsync = GPIO_NUM_51;
constexpr int kTvpReset = 52;     // TVP5150 RESETB (active low, J1 pin 36 / module pin 17)
constexpr int kReverseGpio = 25;  // active low (Q3 pulls it down while the reverse lamps are lit)
constexpr uint8_t kTvpAddr[2] = {0x5C, 0x5D};

// ---------------------------------------------------------------- capture geometry
// BT.656 carries whole lines, blanking included: NTSC 858 samples × 262.5 lines per field, PAL 864 × 312.5.
// With the DVP "data enable" held high every byte is captured; one field fits in 864 × 314 YUV pixels.
constexpr uint32_t kCapW = 864, kCapH = 314;
constexpr size_t kFieldBytes = kCapW * kCapH * 2;  // 542,592: a multiple of the 128-byte cache line
constexpr int kCapBufs = 3;
constexpr int kActiveBytes = 1440;                 // 720 pixels of Cb Y Cr Y
constexpr int kMaxLines = 300;
// The picture fills the 800×800 round glass: full height, the 4:3 picture's sides cropped (the centre 540 of the
// 720 samples) so nothing is stretched.
constexpr int kOutW = 800, kOutH = 800;
constexpr int kCropW = 540, kCropX = (720 - kCropW) / 2;
constexpr int kOutBufs = 3;

// ---------------------------------------------------------------- state
CameraMode s_mode = CameraMode::Auto;
uint8_t s_addr = 0;           // TVP5150 I2C address once found
uint32_t s_last_probe = 0;
esp_cam_ctlr_handle_t s_cam = nullptr;
bool s_running = false;
bool s_vsync_inv = false, s_pclk_inv = false;

uint8_t *s_field[kCapBufs] = {};
volatile size_t s_rx[kCapBufs] = {};
volatile int s_filling = 0;
volatile int s_busy = -1;     // capture buffer being converted
QueueHandle_t s_done_q = nullptr;

uint16_t *s_out[kOutBufs] = {};
portMUX_TYPE s_out_mux = portMUX_INITIALIZER_UNLOCKED;
volatile int s_show = 0;      // buffer on screen
volatile int s_ready = -1;    // newest converted frame not yet on screen

// stats
volatile uint32_t s_fields = 0, s_frames = 0, s_last_lines = 0, s_convert_us = 0, s_last_frame_ms = 0;
uint32_t s_fields_ps = 0, s_frames_ps = 0, s_stat_t0 = 0, s_fields0 = 0, s_frames0 = 0;

// UI
lv_obj_t *s_layer = nullptr, *s_img = nullptr, *s_msg = nullptr;
lv_img_dsc_t s_dsc;
bool s_shown = false;
bool s_rev = false;           // debounced reverse
uint32_t s_rev_edge = 0, s_rev_off_ms = 0;

// YUV → RGB (BT.601, video range) lookup tables
int16_t s_ly[256], s_lrv[256], s_lgu[256], s_lgv[256], s_lbu[256];
uint8_t s_clamp[1024];
uint16_t s_col_y[kOutW], s_col_uv[kOutW];

// ---------------------------------------------------------------- TVP5150
bool tvp_read(uint8_t reg, uint8_t *v) { return s_addr && cluster_i2c_master_read_reg(s_addr, reg, v); }
bool tvp_write(uint8_t reg, uint8_t v) { return s_addr && cluster_i2c_master_write_reg(s_addr, reg, v); }

// RESETB: the board's own RC (10k / 2.2 uF) resets the decoder at power-up; a clean reset from here as well, then the
// pin is let go so the pull-up holds it high (low 10 ms through the board's 1k, then about 26 ms to rise past VIH)
void tvp_reset() {
  pinMode(kTvpReset, OUTPUT);
  digitalWrite(kTvpReset, LOW);
  delay(10);
  pinMode(kTvpReset, INPUT);
  delay(50);
}

bool tvp_probe() {
  for (uint8_t a : kTvpAddr) {
    uint8_t hi = 0, lo = 0;
    if (cluster_i2c_master_read_reg(a, 0x80, &hi) && cluster_i2c_master_read_reg(a, 0x81, &lo) && hi == 0x51 &&
        lo == 0x50) {
      s_addr = a;
      return true;
    }
  }
  return false;
}

void tvp_init() {
  tvp_write(0x00, 0x00);  // input: composite on AIP1A
  tvp_write(0x28, 0x00);  // video standard: autoswitch (NTSC / PAL)
  tvp_write(0x0D, 0x47);  // outputs: ITU-R BT.656, 8-bit 4:2:2, embedded syncs
  tvp_write(0x03, 0x0D);  // enable YCbCr data, sync and clock outputs
}

// ---------------------------------------------------------------- capture (ISR callbacks)
bool IRAM_ATTR on_new_trans(esp_cam_ctlr_handle_t, esp_cam_ctlr_trans_t *trans, void *) {
  int i = (s_filling + 1) % kCapBufs;
  if (i == s_busy) {
    i = (i + 1) % kCapBufs;
  }
  s_filling = i;
  trans->buffer = s_field[i];
  trans->buflen = kFieldBytes;
  return false;
}

bool IRAM_ATTR on_trans_done(esp_cam_ctlr_handle_t, esp_cam_ctlr_trans_t *trans, void *) {
  int i = 0;
  while (i < kCapBufs && s_field[i] != trans->buffer) i++;
  if (i == kCapBufs) {
    return false;
  }
  s_rx[i] = trans->received_size;
  s_fields++;
  BaseType_t woken = pdFALSE;
  xQueueSendFromISR(s_done_q, &i, &woken);
  return woken == pdTRUE;
}

// ---------------------------------------------------------------- conversion task
inline uint16_t yuv565(int y, int u, int v) {
  const int yy = s_ly[y];
  const int r = s_clamp[((yy + s_lrv[v]) >> 8) + 384];
  const int g = s_clamp[((yy - s_lgu[u] - s_lgv[v]) >> 8) + 384];
  const int b = s_clamp[((yy + s_lbu[u]) >> 8) + 384];
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/** Convert one captured field into an output frame. Returns false if it was not a usable top field. */
bool convert_field(const uint8_t *buf, size_t n) {
  static const uint8_t *line[kMaxLines];
  int lines = 0, field = -1;
  const uint8_t *p = buf, *end = buf + (n > 4 ? n - 4 : 0);
  while (p < end && lines < kMaxLines) {
    p = (const uint8_t *)memchr(p, 0xFF, (size_t)(end - p));
    if (!p) break;
    if (p[1] == 0x00 && p[2] == 0x00 && (p[3] & 0x80)) {
      const uint8_t xy = p[3];
      const bool eav = xy & 0x10, vblank = xy & 0x20;
      const int f = (xy >> 6) & 1;
      if (!eav && !vblank) {  // start of an active line
        if (field < 0) field = f;
        if (f == field && p + 4 + kActiveBytes <= buf + n) {
          line[lines++] = p + 4;
        }
        p += 4 + kActiveBytes;
        continue;
      }
      p += 4;
      continue;
    }
    p++;
  }
  s_last_lines = (uint32_t)lines;
  if (field != 0 || lines < 200) {
    return false;  // show top fields only (30 frames/s, no half-line bob), and only complete ones
  }
  int out = -1;
  portENTER_CRITICAL(&s_out_mux);
  for (int i = 0; i < kOutBufs; i++) {
    if (i != s_show && i != s_ready) {
      out = i;
      break;
    }
  }
  portEXIT_CRITICAL(&s_out_mux);
  if (out < 0) {
    return false;
  }
  uint16_t *dst = s_out[out];
  int prev = -1;
  for (int r = 0; r < kOutH; r++) {
    const int sl = r * lines / kOutH;  // 240 → 800 line repeat (PAL 288 → 800)
    uint16_t *row = dst + (size_t)r * kOutW;
    if (sl == prev) {
      memcpy(row, row - kOutW, kOutW * 2);
      continue;
    }
    prev = sl;
    const uint8_t *src = line[sl];
    for (int c = 0; c < kOutW; c++) {
      const uint8_t *uv = src + s_col_uv[c];
      row[c] = yuv565(src[s_col_y[c]], uv[0], uv[2]);
    }
  }
  portENTER_CRITICAL(&s_out_mux);
  s_ready = out;
  portEXIT_CRITICAL(&s_out_mux);
  return true;
}

void convert_task(void *) {
  for (;;) {
    int i;
    if (xQueueReceive(s_done_q, &i, portMAX_DELAY) != pdTRUE) continue;
    int newer;
    while (xQueueReceive(s_done_q, &newer, 0) == pdTRUE) i = newer;  // always the newest field
    s_busy = i;
    esp_cache_msync(s_field[i], kFieldBytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    const int64_t t0 = esp_timer_get_time();
    if (convert_field(s_field[i], s_rx[i] ? s_rx[i] : kFieldBytes)) {
      s_convert_us = (uint32_t)(esp_timer_get_time() - t0);
      s_frames++;
      s_last_frame_ms = millis();
    }
    s_busy = -1;
  }
}

void build_tables() {
  for (int i = 0; i < 256; i++) {
    s_ly[i] = (int16_t)(298 * (i - 16) + 128);
    s_lrv[i] = (int16_t)(409 * (i - 128));
    s_lgu[i] = (int16_t)(100 * (i - 128));
    s_lgv[i] = (int16_t)(208 * (i - 128));
    s_lbu[i] = (int16_t)(516 * (i - 128));
  }
  for (int i = 0; i < 1024; i++) {
    const int v = i - 384;
    s_clamp[i] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
  }
  for (int c = 0; c < kOutW; c++) {
    const int sx = kCropX + c * kCropW / kOutW;  // centre 540 of 720 → 800 (square-ish pixels)
    s_col_uv[c] = (uint16_t)((sx >> 1) * 4);      // Cb of the pair (Cr is +2)
    s_col_y[c] = (uint16_t)(s_col_uv[c] + ((sx & 1) ? 3 : 1));
  }
}

bool capture_create() {
  for (int i = 0; i < kCapBufs; i++) {
    s_field[i] = (uint8_t *)heap_caps_aligned_calloc(128, 1, kFieldBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!s_field[i]) s_field[i] = (uint8_t *)heap_caps_aligned_calloc(128, 1, kFieldBytes, MALLOC_CAP_SPIRAM);
  }
  for (int i = 0; i < kOutBufs; i++) {
    s_out[i] = (uint16_t *)heap_caps_calloc(1, kOutW * kOutH * 2, MALLOC_CAP_SPIRAM);
  }
  for (int i = 0; i < kCapBufs; i++) {
    if (!s_field[i]) return false;
  }
  for (int i = 0; i < kOutBufs; i++) {
    if (!s_out[i]) return false;
  }
  build_tables();
  s_done_q = xQueueCreate(4, sizeof(int));
  xTaskCreatePinnedToCore(convert_task, "camera", 4096, nullptr, 5, nullptr, 0);

  esp_cam_ctlr_dvp_pin_config_t pins = {};
  pins.data_width = CAM_CTLR_DATA_WIDTH_8;
  for (int i = 0; i < CAM_DVP_DATA_SIG_NUM; i++) {
    pins.data_io[i] = i < 8 ? kData[i] : GPIO_NUM_NC;
  }
  pins.vsync_io = kVsync;
  pins.de_io = kHsync;  // re-routed to a constant 1 below: BT.656 keeps its syncs in the data
  pins.pclk_io = kPclk;
  pins.xclk_io = GPIO_NUM_NC;
  esp_cam_ctlr_dvp_config_t cfg = {};
  cfg.ctlr_id = 0;
  cfg.clk_src = CAM_CLK_SRC_DEFAULT;
  cfg.h_res = kCapW;
  cfg.v_res = kCapH;
  cfg.input_data_color_type = CAM_CTLR_COLOR_YUV422;
  cfg.cam_data_width = 8;
  cfg.bk_buffer_dis = 1;
  cfg.external_xtal = 1;  // the module has its own crystal; no XCLK from us
  cfg.dma_burst_size = 128;
  cfg.pin = &pins;
  esp_err_t e = esp_cam_new_dvp_ctlr(&cfg, &s_cam);
  if (e != ESP_OK) {
    Serial.printf("[camera] DVP init failed: %s\n", esp_err_to_name(e));
    s_cam = nullptr;
    return false;
  }
  esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, CAM_H_ENABLE_PAD_IN_IDX, false);
  esp_cam_ctlr_evt_cbs_t cbs = {};
  cbs.on_get_new_trans = on_new_trans;
  cbs.on_trans_finished = on_trans_done;
  esp_cam_ctlr_register_event_callbacks(s_cam, &cbs, nullptr);
  esp_cam_ctlr_enable(s_cam);
  Serial.printf("[camera] TVP5150 at 0x%02X, DVP ready (%u KB per field ×%d)\n", s_addr, (unsigned)(kFieldBytes / 1024),
                kCapBufs);
  return true;
}

void capture_run(bool on) {
  if (!s_cam || on == s_running) {
    return;
  }
  // Stop capturing while the view is hidden: PSRAM bandwidth is what the gauges live on.
  if (on) {
    esp_cam_ctlr_start(s_cam);
  } else {
    esp_cam_ctlr_stop(s_cam);
  }
  s_running = on;
}

// ---------------------------------------------------------------- UI
void line(lv_obj_t *parent, const lv_point_t *pts, int n, uint32_t color) {
  lv_obj_t *l = lv_line_create(parent);
  lv_line_set_points(l, pts, (uint16_t)n);
  lv_obj_set_style_line_width(l, 6, 0);
  lv_obj_set_style_line_rounded(l, true, 0);
  lv_obj_set_style_line_color(l, lv_color_hex(color), 0);
  lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
}

void build_ui() {
  lv_obj_t *parent = p4_screen_ui_parent();
  const int w = lv_disp_get_hor_res(nullptr), h = lv_disp_get_ver_res(nullptr);
  s_layer = lv_obj_create(parent);
  lv_obj_remove_style_all(s_layer);
  lv_obj_set_size(s_layer, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(s_layer, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(s_layer, LV_OPA_COVER, 0);
  lv_obj_clear_flag(s_layer, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(s_layer, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_layer, LV_OBJ_FLAG_HIDDEN);

  s_dsc = {};
  s_dsc.header.always_zero = 0;
  s_dsc.header.w = kOutW;
  s_dsc.header.h = kOutH;
  s_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
  s_dsc.data_size = kOutW * kOutH * 2;
  s_dsc.data = (const uint8_t *)s_out[0];
  s_img = lv_img_create(s_layer);
  if (s_out[0]) {
    lv_img_set_src(s_img, &s_dsc);
  }
  const int x0 = (w - kOutW) / 2, y0 = (h - kOutH) / 2;
  lv_obj_set_pos(s_img, x0, y0);
  lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);

  // Parking guides, laid out on a 640×480 view of the whole 4:3 picture and mapped onto the cropped, full-screen
  // one so they stay put on the scene: red near, yellow mid, green far. The near red ends run off the glass.
  static lv_point_t lr[2], ly[2], lg[2], rr[2], ry[2], rg[2], cr[2], cy[2], cg[2];
  auto gx = [&](int x) { return (lv_coord_t)(x0 + (x * 720 / 640 - kCropX) * kOutW / kCropW); };
  auto gy = [&](int y) { return (lv_coord_t)(y0 + y * kOutH / 480); };
  auto P = [&](lv_point_t *a, int ax, int ay, int bx, int by) {
    a[0] = {gx(ax), gy(ay)};
    a[1] = {gx(bx), gy(by)};
  };
  P(lr, 120, 470, 170, 400); P(ly, 170, 400, 215, 330); P(lg, 215, 330, 250, 270);
  P(rr, 520, 470, 470, 400); P(ry, 470, 400, 425, 330); P(rg, 425, 330, 390, 270);
  P(cr, 170, 400, 200, 400); P(cy, 215, 330, 240, 330); P(cg, 250, 270, 270, 270);
  static lv_point_t cr2[2], cy2[2], cg2[2];
  P(cr2, 440, 400, 470, 400); P(cy2, 400, 330, 425, 330); P(cg2, 370, 270, 390, 270);
  line(s_layer, lr, 2, 0xFF3B30); line(s_layer, rr, 2, 0xFF3B30); line(s_layer, cr, 2, 0xFF3B30); line(s_layer, cr2, 2, 0xFF3B30);
  line(s_layer, ly, 2, 0xFFCC00); line(s_layer, ry, 2, 0xFFCC00); line(s_layer, cy, 2, 0xFFCC00); line(s_layer, cy2, 2, 0xFFCC00);
  line(s_layer, lg, 2, 0x45D483); line(s_layer, rg, 2, 0x45D483); line(s_layer, cg, 2, 0x45D483); line(s_layer, cg2, 2, 0x45D483);

  s_msg = lv_label_create(s_layer);
  lv_obj_set_style_text_font(s_msg, &elc_sm_28, 0);
  lv_obj_set_style_text_color(s_msg, lv_color_hex(0xF3F5F6), 0);
  lv_obj_set_style_text_align(s_msg, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_bg_color(s_msg, lv_color_black(), 0);  // over the picture now: a dark pill keeps it legible
  lv_obj_set_style_bg_opa(s_msg, LV_OPA_50, 0);
  lv_obj_set_style_radius(s_msg, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_pad_hor(s_msg, 20, 0);
  lv_obj_set_style_pad_ver(s_msg, 6, 0);
  lv_obj_align(s_msg, LV_ALIGN_TOP_MID, 0, 56);
  lv_label_set_text(s_msg, "REAR CAMERA");
}

void set_msg(const char *m) {
  if (strcmp(lv_label_get_text(s_msg), m) != 0) {
    lv_label_set_text(s_msg, m);
  }
}

}  // namespace

// ---------------------------------------------------------------- public

void center_camera_begin() {
  pinMode(kReverseGpio, INPUT_PULLUP);
  tvp_reset();
  if (tvp_probe()) {
    tvp_init();
    capture_create();
  }
  build_ui();
  s_last_probe = millis();
  s_stat_t0 = millis();
}

void center_camera_loop(uint32_t now) {
  // Hot-plug: look for the module every 5 s until it answers.
  if (!s_addr && now - s_last_probe > 5000u) {
    s_last_probe = now;
    if (tvp_probe()) {
      tvp_init();
      capture_create();
    }
  }
  // Reverse input: 100 ms debounce on, 1.5 s hold after leaving reverse.
  const bool raw = digitalRead(kReverseGpio) == LOW;
  if (raw != s_rev) {
    if (!s_rev_edge) s_rev_edge = now;
    if (now - s_rev_edge >= (raw ? 100u : 1500u)) {
      s_rev = raw;
      s_rev_edge = 0;
    }
  } else {
    s_rev_edge = 0;
  }
  const bool want = s_mode == CameraMode::On || (s_mode == CameraMode::Auto && s_rev);
  if (want != s_shown) {
    s_shown = want;
    if (want) {
      lv_obj_clear_flag(s_layer, LV_OBJ_FLAG_HIDDEN);
      lv_obj_move_foreground(s_layer);
    } else {
      lv_obj_add_flag(s_layer, LV_OBJ_FLAG_HIDDEN);
    }
    capture_run(want);
  }
  if (now - s_stat_t0 >= 1000u) {
    s_fields_ps = s_fields - s_fields0;
    s_frames_ps = s_frames - s_frames0;
    s_fields0 = s_fields;
    s_frames0 = s_frames;
    s_stat_t0 = now;
  }
  if (!s_shown) {
    return;
  }
  int ready;
  portENTER_CRITICAL(&s_out_mux);
  ready = s_ready;
  if (ready >= 0) {
    s_show = ready;
    s_ready = -1;
  }
  portEXIT_CRITICAL(&s_out_mux);
  if (ready >= 0) {
    s_dsc.data = (const uint8_t *)s_out[ready];
    lv_obj_clear_flag(s_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(s_img);
  }
  const bool live = s_last_frame_ms && now - s_last_frame_ms < 500u;
  if (!s_addr) {
    set_msg("NO VIDEO MODULE");
  } else if (!live) {
    set_msg("NO CAMERA SIGNAL");
  } else {
    set_msg("REAR CAMERA");
  }
  if (!live && !lv_obj_has_flag(s_img, LV_OBJ_FLAG_HIDDEN) && s_last_frame_ms && now - s_last_frame_ms > 1500u) {
    lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);  // don't freeze on a stale picture
  }
}

bool center_camera_showing() { return s_shown; }

bool center_camera_reverse() { return s_rev; }

void center_camera_set_mode(CameraMode mode) { s_mode = mode; }

void center_camera_status(char *out, size_t n) {
  uint8_t s1 = 0, s5 = 0;
  const bool st = tvp_read(0x88, &s1) && tvp_read(0x8C, &s5);
  snprintf(out, n,
           "module %s%s, signal %s (status 0x%02X/0x%02X), reverse %s, mode %s, view %s, "
           "capture %s: %lu fields/s, %lu frames/s, %lu active lines, convert %.1f ms, vsync%s pclk%s",
           s_addr ? "TVP5150 @0x" : "not found", s_addr ? (s_addr == 0x5C ? "5C" : "5D") : "",
           !st ? "?" : ((s1 & 0x06) == 0x06 ? "locked" : "no lock"), s1, s5, s_rev ? "ON" : "off",
           s_mode == CameraMode::On ? "on" : (s_mode == CameraMode::Off ? "off" : "auto"), s_shown ? "shown" : "hidden",
           s_running ? "running" : "stopped", (unsigned long)s_fields_ps, (unsigned long)s_frames_ps,
           (unsigned long)s_last_lines, (double)s_convert_us / 1000.0, s_vsync_inv ? " inverted" : "",
           s_pclk_inv ? " inverted" : "");
}

void center_camera_toggle_vsync_invert() {
  s_vsync_inv = !s_vsync_inv;
  esp_rom_gpio_connect_in_signal(kVsync, CAM_V_SYNC_PAD_IN_IDX, s_vsync_inv);
}

void center_camera_toggle_pclk_invert() {
  s_pclk_inv = !s_pclk_inv;
  esp_rom_gpio_connect_in_signal(kPclk, CAM_PCLK_PAD_IN_IDX, s_pclk_inv);
}

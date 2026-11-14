#include "center_camera.h"

#include <Arduino.h>
#include <cmath>
#include <cstring>

#include <cluster_i2c.h>
#include <elc_fonts.h>
#include <esp_cache.h>
#include <esp_cam_ctlr.h>
#include <esp_cam_ctlr_dvp.h>
#include <esp_heap_caps.h>
#include <driver/gpio.h>
#include <driver/ppa.h>
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
#ifndef ELC_REVERSE_GPIO
#define ELC_REVERSE_GPIO 25
#endif
// Active low (Q3 pulls it down while the reverse lamps are lit). -1 = no reverse input (4C: GPIO25 is USB D+).
constexpr int kReverseGpio = ELC_REVERSE_GPIO;
constexpr uint8_t kTvpAddr[2] = {0x5C, 0x5D};

// ---------------------------------------------------------------- capture geometry
// BT.656 carries whole lines, blanking included: NTSC 858 samples × 262.5 lines per field, PAL 864 × 312.5.
// With the DVP "data enable" held high every byte is captured; one field fits in 864 × 314 YUV pixels. (Buffers of
// two fields were tried against the capture stalls, 2026-11-06: no VSYNC edge was ever missed, the stalls stayed.)
constexpr uint32_t kCapW = 864, kCapH = 314;
constexpr size_t kFieldBytes = kCapW * kCapH * 2;  // 542,592: a multiple of the 128-byte cache line
constexpr int kCapBufs = 3;
constexpr int kActiveBytes = 1440;                 // 720 pixels of Cb Y Cr Y
constexpr int kMaxLines = 300;
// The picture fills the 800×800 round glass: full height, the 4:3 picture's sides cropped (the centre 540 of the
// 720 samples) so nothing is stretched.
// The CPU converts the colour of each source pixel once, at native size, into s_rgb; the PPA (pixel-processing
// accelerator) then scales that up into the output frame by DMA. Its scale steps are 1/16 and it takes YUV only from
// chip revision 3 on (the 3.4C is an earlier chip), hence RGB565 in and these sizes:
//   width:  the centre 532 of the 720 samples × 1.5      = 798 of the 800 columns
//   height: NTSC 240 lines × 3.3125 = 795 rows, PAL 288 lines × 2.75 = 792 rows; centred, the few rows left stay black.
constexpr int kOutW = 800, kOutH = 800;
constexpr size_t kOutBytes = (size_t)kOutW * kOutH * 2;  // 1,280,000: a multiple of the 128-byte cache line
constexpr int kCropW = 532, kCropX = (720 - kCropW) / 2;  // 94: even, so each pixel pair shares its Cb / Cr
constexpr float kScaleX = 1.5f;
constexpr int kPicW = 798, kPicX = (kOutW - kPicW) / 2;
constexpr int kNtscH = 240, kPalH = 288;
constexpr float kScaleNtsc = 3.3125f, kScalePal = 2.75f;
constexpr int kNtscRows = 795, kPalRows = 792;
constexpr int kOutBufs = 3;

// ---------------------------------------------------------------- state
CameraMode s_mode = CameraMode::Auto;
uint8_t s_addr = 0;           // TVP5150 I2C address once found
uint32_t s_last_probe = 0;
esp_cam_ctlr_handle_t s_cam = nullptr;
bool s_running = false;
bool s_vsync_inv = false, s_pclk_inv = false;

// Triple buffering. At any moment each capture buffer is at most one of: being filled by the DMA, the newest complete
// field (not yet taken), or being converted. Both sides change these under s_cap_mux (the ISR and the conversion task
// run on different cores), so the DMA is never handed a buffer the converter is reading.
uint8_t *s_field[kCapBufs] = {};
portMUX_TYPE s_cap_mux = portMUX_INITIALIZER_UNLOCKED;
int s_filling = 0;            // handed to the DMA
int s_latest = -1;            // newest complete field, not yet taken
int s_busy = -1;              // being converted
TaskHandle_t s_conv_task = nullptr;

uint16_t *s_out[kOutBufs] = {};
portMUX_TYPE s_out_mux = portMUX_INITIALIZER_UNLOCKED;
volatile int s_show = 0;      // buffer on screen
volatile int s_ready = -1;    // newest converted frame not yet on screen

// stats
volatile uint32_t s_fields = 0, s_frames = 0, s_last_lines = 0, s_convert_us = 0, s_last_frame_ms = 0;
volatile uint32_t s_scan_us = 0, s_csc_us = 0, s_scale_us = 0;
volatile uint32_t s_partial = 0;  // top fields dropped as incomplete  // last frame's stages: find lines, colour, scale
uint32_t s_fields_ps = 0, s_frames_ps = 0, s_stat_t0 = 0, s_fields0 = 0, s_frames0 = 0;
uint32_t s_field_seen = 0, s_field_ms = 0, s_restarts = 0;  // capture watchdog
// Field timing (µs between field ends): NTSC is 16,683, PAL 20,000; much shorter means a glitch on VSYNC.
volatile int64_t s_field_us = 0;
volatile uint32_t s_gap_min = UINT32_MAX, s_gap_max = 0, s_gap_short = 0, s_gap_long = 0;

// UI
lv_obj_t *s_layer = nullptr, *s_img = nullptr, *s_msg = nullptr;
lv_img_dsc_t s_dsc;
bool s_shown = false;
bool s_rev = false;           // debounced reverse
uint32_t s_rev_edge = 0, s_rev_off_ms = 0;

// Data-line check (`cam bits`): the console asks, the conversion task analyses the next field it takes.
struct BitReport {
  uint32_t bytes, codes_ok, codes_bad, active_sav, blank_bytes;
  uint32_t ones[8], toggles[8];
  uint32_t err_hi[8], err_lo[8];  // blanking bytes with this bit read 1 where 0 was sent / read 0 where 1 was sent
  uint32_t code_err[8];           // bad timing codes one bit away from a valid one: that bit
  uint8_t sample[16];
  uint64_t cb_dev, cr_dev;   // summed |Cb - 128|, |Cr - 128| over active lines
  uint32_t chroma_n, chroma_strong;  // chroma samples, and those more than 16 from neutral
  int64_t cb_sum[8], cr_sum[8];  // signed Cb - 128, Cr - 128 by eighth of the line
  uint64_t cb_abs[8];
  uint32_t bin_n[8];
  int sample_len;
};
BitReport s_bits;
int s_bits_state = 0;  // 0 idle, 1 asked, 2 report ready (__atomic: the report is written on the other core)

// YUV → RGB (BT.601, video range) lookup tables
// 32-bit: the scaled terms reach ±71,000 (16 bits wrapped every pixel brighter than Y 126, and strong colours too).
int32_t s_ly[256], s_lrv[256], s_lgu[256], s_lgv[256], s_lbu[256];
uint8_t s_clamp[1024];
uint16_t *s_rgb = nullptr;           // native-size RGB565 picture (kCropW × up to kPalH) the PPA scales up
ppa_client_handle_t s_ppa = nullptr;  // null: software scaling instead

// Direct mode (p4_screen_set_direct): while the picture is live the PPA scales it straight into the screen's hidden
// framebuffer and the CPU lays the guides and label over it, so LVGL never copies the full-screen picture. The overlay
// is drawn once at start-up into runs of pixels with their colour and opacity; each frame blends just those.
struct OvRun {
  uint16_t y, x, n;
  uint32_t at;  // index of the run's first pixel in s_ov_col / s_ov_alpha
};
OvRun *s_ov_run = nullptr;
uint16_t *s_ov_col = nullptr;
uint8_t *s_ov_alpha = nullptr;
int s_ov_runs = 0;
bool s_direct_on = false;                 // LVGL loop: direct mode is on
volatile uint32_t s_direct_frames = 0;    // frames shown directly
volatile uint32_t s_finish_us = 0, s_show_us = 0;  // last direct frame: borders + overlay + writeback; flip

// Parking guides on a 640×480 view of the whole 4:3 picture, mapped onto the cropped full-screen one so they stay put
// on the scene: red near, yellow mid, green far (the near red ends run off the glass). LVGL draws them over the
// picture it shows; direct mode bakes the same lines into the overlay.
struct Guide {
  int16_t ax, ay, bx, by;
  uint32_t color;
};
constexpr Guide kGuides[] = {
    {120, 470, 170, 400, 0xFF3B30}, {520, 470, 470, 400, 0xFF3B30}, {170, 400, 200, 400, 0xFF3B30},
    {440, 400, 470, 400, 0xFF3B30}, {170, 400, 215, 330, 0xFFCC00}, {470, 400, 425, 330, 0xFFCC00},
    {215, 330, 240, 330, 0xFFCC00}, {400, 330, 425, 330, 0xFFCC00}, {215, 330, 250, 270, 0x45D483},
    {425, 330, 390, 270, 0x45D483}, {250, 270, 270, 270, 0x45D483}, {370, 270, 390, 270, 0x45D483},
};
constexpr int kNumGuides = sizeof(kGuides) / sizeof(kGuides[0]);
constexpr int kGuideWidth = 6;
inline int guide_x(int x) { return kPicX + (x * 720 / 640 - kCropX) * kPicW / kCropW; }
inline int guide_y(int y) { return (kOutH - kNtscRows) / 2 + y * kNtscRows / 480; }
// The status pill: elc_sm_28 text on half-opaque black, padded 20 / 6, its top 56 px down, centred.
constexpr int kMsgTop = 56, kMsgPadH = 20, kMsgPadV = 6;
constexpr uint32_t kMsgText = 0xF3F5F6;

// ---------------------------------------------------------------- TVP5150
bool tvp_read(uint8_t reg, uint8_t *v) { return s_addr && cluster_i2c_master_read_reg(s_addr, reg, v); }
bool tvp_write(uint8_t reg, uint8_t v) { return s_addr && cluster_i2c_master_write_reg(s_addr, reg, v); }

// RESETB: the board's own RC (10k / 2.2 uF) resets the decoder at power-up; a clean reset from here as well, then the
// pin is let go so the pull-up holds it high (low 10 ms through the board's 1k, then about 26 ms to rise past VIH).
// The internal pull-up as well, so a module wired straight to the header (no board RC) still comes out of reset.
void tvp_reset() {
  pinMode(kTvpReset, OUTPUT);
  digitalWrite(kTvpReset, LOW);
  delay(10);
  pinMode(kTvpReset, INPUT_PULLUP);
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
// The driver asks for the next buffer first and only then reports the one just filled (still s_filling here), so the
// new buffer is the one that is neither that nor the converter's: with three, one is always left.
bool IRAM_ATTR on_new_trans(esp_cam_ctlr_handle_t, esp_cam_ctlr_trans_t *trans, void *) {
  portENTER_CRITICAL_ISR(&s_cap_mux);
  int i = 0;
  while (i == s_filling || i == s_busy) i++;
  s_filling = i;
  portEXIT_CRITICAL_ISR(&s_cap_mux);
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
  portENTER_CRITICAL_ISR(&s_cap_mux);
  s_latest = i;  // supersedes an untaken older field
  portEXIT_CRITICAL_ISR(&s_cap_mux);
  const int64_t t = esp_timer_get_time();
  if (s_field_us) {
    const uint32_t gap = (uint32_t)(t - s_field_us);
    if (gap < s_gap_min) s_gap_min = gap;
    if (gap > s_gap_max) s_gap_max = gap;
    if (gap < 15000u) s_gap_short++;
    if (gap > 25000u) s_gap_long++;  // a VSYNC edge missed (or a field lost some other way)
  }
  s_field_us = t;
  s_fields++;
  BaseType_t woken = pdFALSE;
  if (s_conv_task) vTaskNotifyGiveFromISR(s_conv_task, &woken);
  return woken == pdTRUE;
}

// ---------------------------------------------------------------- conversion task
// The build optimises for size (-Os); the per-pixel loops below run 130,000 times a frame, so they get -O2.
#pragma GCC push_options
#pragma GCC optimize("O2")

// One pixel from its luma term and the chroma terms its pair shares.
inline uint16_t rgb565(int yy, int rv, int guv, int bu) {
  const int r = s_clamp[((yy + rv) >> 8) + 384];
  const int g = s_clamp[((yy - guv) >> 8) + 384];
  const int b = s_clamp[((yy + bu) >> 8) + 384];
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/** One active line (Cb Y Cr Y …) to kCropW RGB565 pixels: the chroma terms once per pair. */
void convert_line(const uint8_t *src, uint16_t *row) {
  src += kCropX * 2;
  for (int c = 0; c < kCropW; c += 2, src += 4) {
    const int cb = src[0], cr = src[2];
    const int rv = s_lrv[cr], guv = s_lgu[cb] + s_lgv[cr], bu = s_lbu[cb];
    row[c] = rgb565(s_ly[src[1]], rv, guv, bu);
    row[c + 1] = rgb565(s_ly[src[3]], rv, guv, bu);
  }
}

#pragma GCC pop_options

/** s_rgb (height lines) into dst, centred: the PPA, or the CPU if the PPA is missing or refuses. */
void scale_to(uint16_t *dst, int height) {
  const bool pal = height == kPalH;
  const int rows = pal ? kPalRows : kNtscRows, y0 = (kOutH - rows) / 2;
  if (s_ppa) {
    ppa_srm_oper_config_t op = {};
    op.in.buffer = s_rgb;
    op.in.pic_w = kCropW;
    op.in.pic_h = (uint32_t)height;
    op.in.block_w = kCropW;
    op.in.block_h = (uint32_t)height;
    op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    op.out.buffer = dst;
    op.out.buffer_size = kOutBytes;
    op.out.pic_w = kOutW;
    op.out.pic_h = kOutH;
    op.out.block_offset_x = kPicX;
    op.out.block_offset_y = (uint32_t)y0;
    op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    op.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
    op.scale_x = kScaleX;
    op.scale_y = pal ? kScalePal : kScaleNtsc;
    op.mode = PPA_TRANS_MODE_BLOCKING;
    if (ppa_do_scale_rotate_mirror(s_ppa, &op) == ESP_OK) return;
  }
  int prev = -1;
  for (int r = 0; r < rows; r++) {
    const int sl = r * height / rows;
    uint16_t *row = dst + (size_t)(y0 + r) * kOutW + kPicX;
    if (sl == prev) {
      memcpy(row, row - kOutW, kPicW * 2);
      continue;
    }
    prev = sl;
    const uint16_t *src = s_rgb + (size_t)sl * kCropW;
    for (int c = 0; c < kPicW; c++) row[c] = src[c * kCropW / kPicW];
  }
}

/** Blend s (opacity a, 0–255) over d: both RGB565, all three channels at once (green spread to the top half). */
inline uint16_t mix565(uint16_t s, uint16_t d, uint32_t a) {
  if (a >= 252) return s;
  const uint32_t a5 = (a + 4) >> 3;  // 0..32
  const uint32_t s32 = (s | ((uint32_t)s << 16)) & 0x07E0F81Fu, d32 = (d | ((uint32_t)d << 16)) & 0x07E0F81Fu;
  const uint32_t r = ((s32 * a5 + d32 * (32 - a5)) >> 5) & 0x07E0F81Fu;
  return (uint16_t)(r | (r >> 16));
}

/**
 * A direct frame after the PPA: black where the picture does not reach (the framebuffer last held LVGL's screen), the
 * overlay blended on, and everything the CPU wrote pushed out of the cache before the panel shows it.
 */
void finish_direct(uint16_t *fb, int height) {
  const int rows = height == kPalH ? kPalRows : kNtscRows, y0 = (kOutH - rows) / 2, y1 = y0 + rows;
  memset(fb, 0, (size_t)y0 * kOutW * 2);
  memset(fb + (size_t)y1 * kOutW, 0, (size_t)(kOutH - y1) * kOutW * 2);
  for (int y = y0; y < y1; y++) {
    uint16_t *row = fb + (size_t)y * kOutW;
    for (int x = 0; x < kPicX; x++) row[x] = 0;
    for (int x = kPicX + kPicW; x < kOutW; x++) row[x] = 0;
  }
  for (int i = 0; i < s_ov_runs; i++) {
    const OvRun &r = s_ov_run[i];
    uint16_t *d = fb + (size_t)r.y * kOutW + r.x;
    const uint16_t *c = s_ov_col + r.at;
    const uint8_t *a = s_ov_alpha + r.at;
    for (int k = 0; k < r.n; k++) d[k] = mix565(c[k], d[k], a[k]);
  }
  esp_cache_msync(fb, kOutBytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
}

/**
 * Convert one captured field into an output frame. Returns false if it was not a usable top field.
 *
 * Every line goes to the row its position in the field says (its byte offset from the first active line, in whole
 * line periods), not to the next free row: a line whose sync code was lost leaves a gap that the line above fills,
 * so the picture keeps its size and place from frame to frame instead of stretching whatever lines were found.
 */
bool convert_field(const uint8_t *buf, size_t n) {
  const int64_t t_scan = esp_timer_get_time();
  static const uint8_t *slot[kMaxLines];
  memset(slot, 0, sizeof(slot));
  int lines = 0, field = -1, span = 0;  // span: one past the last line's position
  const uint8_t *first = nullptr;
  size_t period = 0;  // bytes per line: 1716 NTSC (858 samples × 2), 1728 PAL; measured from adjacent lines
  const uint8_t *prev_sav = nullptr;
  const uint8_t *p = buf, *end = buf + (n > 4 ? n - 4 : 0);
  while (p < end) {
    p = (const uint8_t *)memchr(p, 0xFF, (size_t)(end - p));
    if (!p) break;
    if (p[1] == 0x00 && p[2] == 0x00 && (p[3] & 0x80)) {
      const uint8_t xy = p[3];
      const bool eav = xy & 0x10, vblank = xy & 0x20;
      const int f = (xy >> 6) & 1;
      if (vblank && first) break;  // the field's closing blanking: whatever follows is an older field's leftovers
      if (!eav && !vblank) {       // start of an active line
        if (field < 0) field = f;
        if (f != field) break;
        if (!first) first = p;
        if (prev_sav && !period) {
          const size_t d = (size_t)(p - prev_sav);
          if (d >= 1700 && d <= 1740) period = d;
        }
        prev_sav = p;
        const size_t idx = (size_t)(p - first + (period ? period : 1716) / 2) / (period ? period : 1716);
        if (idx < (size_t)kMaxLines && p + 4 + kActiveBytes <= buf + n) {
          if (!slot[idx]) lines++;
          slot[idx] = p + 4;
          if ((int)idx + 1 > span) span = (int)idx + 1;
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
  const int64_t t_csc = esp_timer_get_time();
  const int height = period >= 1722 ? kPalH : kNtscH;  // active lines per field
  if (field != 0) {
    return false;  // show top fields only (30 frames/s, no half-line bob)
  }
  // Only whole fields: one captured from partway down (the first after a capture restart, or a glitch on VSYNC) would
  // be drawn shifted and smeared for a frame, a flicker. A few lost lines are filled from their neighbours.
  if (span < height || lines < height - 8) {
    s_partial++;
    return false;
  }
  // The decoder sends a few more active lines than the picture has (NTSC 244 for 240): the first carry VBI data such as
  // closed captions, a coloured noise stripe along the top. Keep the last `height` lines, ending at the field's last.
  const int skip = span > height ? span - height : 0;
  const uint8_t **ln = slot + skip;
  // Fill gaps from the line above (or below, at the top) so every row has a source line.
  int top = 0;
  while (top < height && !ln[top]) top++;
  for (int k = 0; k < top; k++) ln[k] = ln[top];
  for (int k = top + 1; k < height; k++) {
    if (!ln[k]) ln[k] = ln[k - 1];
  }
  for (int k = 0; k < height; k++) {
    uint16_t *row = s_rgb + (size_t)k * kCropW;
    if (k > 0 && ln[k] == ln[k - 1]) {
      memcpy(row, row - kCropW, kCropW * 2);  // a lost line, filled from the one above
    } else {
      convert_line(ln[k], row);
    }
  }
  const int64_t t_scale = esp_timer_get_time();
  s_scan_us = (uint32_t)(t_csc - t_scan);
  s_csc_us = (uint32_t)(t_scale - t_csc);
  if (uint16_t *fb = p4_screen_direct_begin()) {
    // Straight into the screen's hidden framebuffer, then shown: LVGL is paused while the picture is live.
    scale_to(fb, height);
    const int64_t t_finish = esp_timer_get_time();
    s_scale_us = (uint32_t)(t_finish - t_scale);
    finish_direct(fb, height);
    const int64_t t_show = esp_timer_get_time();
    s_finish_us = (uint32_t)(t_show - t_finish);
    p4_screen_direct_present();
    s_show_us = (uint32_t)(esp_timer_get_time() - t_show);
    s_direct_frames++;
    return true;
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
  scale_to(s_out[out], height);
  s_scale_us = (uint32_t)(esp_timer_get_time() - t_scale);
  portENTER_CRITICAL(&s_out_mux);
  s_ready = out;
  portEXIT_CRITICAL(&s_out_mux);
  return true;
}

// BT.656 timing reference FF 00 00 XY: XY = 1 F V H P3 P2 P1 P0, the P bits protecting F, V and H.
bool xy_valid(uint8_t xy) {
  const int f = (xy >> 6) & 1, v = (xy >> 5) & 1, h = (xy >> 4) & 1;
  const uint8_t p = (uint8_t)(((v ^ h) << 3) | ((f ^ h) << 2) | ((f ^ v) << 1) | (f ^ v ^ h));
  return (xy & 0x80) && (xy & 0x0F) == p;
}

/**
 * Check the 8 data lines on one raw field. Every bit's share of ones and how often it changes (a line that never
 * changes is stuck or open); the timing codes whose protection bits check out; and horizontal blanking, which the
 * TVP5150 sends as a fixed 80 10 80 10 … after every EAV, compared bit by bit: a noisy line shows scattered errors, a
 * swapped pair shows two bits wrong together, mostly on bits 7 and 4 (the ones blanking exercises).
 */
void analyze_bits(const uint8_t *buf, size_t n) {
  static uint32_t hist[256], xhist[256];  // static: this runs on the conversion task's small stack
  constexpr int kBlankCheck = 256;        // horizontal blanking is 268 bytes (NTSC) / 280 (PAL) after the EAV
  memset(hist, 0, sizeof(hist));
  memset(xhist, 0, sizeof(xhist));
  BitReport r = {};
  bool active_seen = false;
  const uint8_t *field_end = buf + n;
  const uint8_t *p = buf, *end = buf + n - 4;
  while (p < end) {
    p = (const uint8_t *)memchr(p, 0xFF, (size_t)(end - p));
    if (!p) break;
    if (p[1] != 0x00 || p[2] != 0x00) {
      p++;
      continue;
    }
    uint8_t xy = p[3];
    if (!xy_valid(xy)) {
      r.codes_bad++;
      // One bit from a valid code (eight exist: F, V, H): that data line is the likely culprit, and the corrected code
      // still marks the line, so blanking after a corrected EAV is checked too.
      bool fixed = false;
      for (int fvh = 0; fvh < 8 && !fixed; fvh++) {
        const uint8_t f = (fvh >> 2) & 1, v = (fvh >> 1) & 1, h = fvh & 1;
        const uint8_t code = (uint8_t)(0x80 | (f << 6) | (v << 5) | (h << 4) | ((v ^ h) << 3) | ((f ^ h) << 2) |
                                       ((f ^ v) << 1) | (f ^ v ^ h));
        const uint8_t d = xy ^ code;
        if (d && !(d & (d - 1))) {
          r.code_err[__builtin_ctz(d)]++;
          xy = code;
          fixed = true;
        }
      }
      if (!fixed) {
        p += 4;
        continue;
      }
    } else {
      r.codes_ok++;
    }
    // The field's closing vertical blanking: the rest of the buffer is an older field's leftovers (the DMA stops at
    // VSYNC, short of the buffer's end), so stop here, as the converter does.
    if ((xy & 0x20) && active_seen) {
      field_end = p;
      break;
    }
    if (!(xy & 0x30)) {  // SAV of an active line
      r.active_sav++;
      active_seen = true;
      if (p + 4 + kActiveBytes <= buf + n) {  // Cb Y Cr Y …: how far the colour strays from neutral grey
        const uint8_t *c = p + 4;
        for (int j = 0; j < kActiveBytes; j += 4) {
          const int db = c[j] > 128 ? c[j] - 128 : 128 - c[j], dr = c[j + 2] > 128 ? c[j + 2] - 128 : 128 - c[j + 2];
          r.cb_dev += (uint64_t)db;
          r.cr_dev += (uint64_t)dr;
          r.chroma_n++;
          const int bin = j * 8 / kActiveBytes;
          r.cb_sum[bin] += c[j] - 128;
          r.cr_sum[bin] += c[j + 2] - 128;
          r.cb_abs[bin] += (uint64_t)db;
          r.bin_n[bin]++;
          if (db > 16 || dr > 16) r.chroma_strong++;
        }
      }
    }
    if (xy & 0x10) {                    // EAV: horizontal blanking follows
      const uint8_t *q = p + 4;
      const int len = (int)((size_t)(buf + n - q) < (size_t)kBlankCheck ? (size_t)(buf + n - q) : (size_t)kBlankCheck);
      for (int j = 0; j < len; j++) {
        const uint8_t sent = (j & 1) ? 0x10 : 0x80, got = q[j], diff = sent ^ got;
        if (!diff) continue;
        for (int bit = 0; bit < 8; bit++) {
          if ((diff >> bit) & 1) ((got >> bit) & 1 ? r.err_hi : r.err_lo)[bit]++;
        }
      }
      r.blank_bytes += (uint32_t)len;
      if (!r.sample_len) {
        r.sample_len = (int)((size_t)(buf + n - p) < sizeof(r.sample) ? (size_t)(buf + n - p) : sizeof(r.sample));
        memcpy(r.sample, p, (size_t)r.sample_len);
      }
    }
    p += 4;
  }
  const size_t len = (size_t)(field_end - buf);
  r.bytes = (uint32_t)len;
  uint8_t prev = buf[0];
  for (size_t k = 0; k < len; k++) {
    const uint8_t b = buf[k];
    hist[b]++;
    xhist[b ^ prev]++;
    prev = b;
  }
  for (int v = 0; v < 256; v++) {
    for (int bit = 0; bit < 8; bit++) {
      if ((v >> bit) & 1) {
        r.ones[bit] += hist[v];
        r.toggles[bit] += xhist[v];
      }
    }
  }
  s_bits = r;
}

void convert_task(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    // Take the newest complete field; from here until it is released the DMA is never handed this buffer.
    portENTER_CRITICAL(&s_cap_mux);
    const int i = s_latest;
    s_latest = -1;
    s_busy = i;
    portEXIT_CRITICAL(&s_cap_mux);
    if (i < 0) continue;
    // The driver always reports the whole buffer as received; the field's own end is found in the data.
    esp_cache_msync(s_field[i], kFieldBytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    if (__atomic_load_n(&s_bits_state, __ATOMIC_ACQUIRE) == 1) {
      analyze_bits(s_field[i], kFieldBytes);
      __atomic_store_n(&s_bits_state, 2, __ATOMIC_RELEASE);
    }
    const int64_t t0 = esp_timer_get_time();
    if (convert_field(s_field[i], kFieldBytes)) {
      s_convert_us = (uint32_t)(esp_timer_get_time() - t0);
      s_frames++;
      s_last_frame_ms = millis();
    }
    portENTER_CRITICAL(&s_cap_mux);
    s_busy = -1;
    portEXIT_CRITICAL(&s_cap_mux);
  }
}

void build_tables() {
  for (int i = 0; i < 256; i++) {
    s_ly[i] = (298 * (i - 16) + 128);
    s_lrv[i] = (409 * (i - 128));
    s_lgu[i] = (100 * (i - 128));
    s_lgv[i] = (208 * (i - 128));
    s_lbu[i] = (516 * (i - 128));
  }
  for (int i = 0; i < 1024; i++) {
    const int v = i - 384;
    s_clamp[i] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
  }
}

bool capture_create() {
  for (int i = 0; i < kCapBufs; i++) {
    s_field[i] = (uint8_t *)heap_caps_aligned_calloc(128, 1, kFieldBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!s_field[i]) s_field[i] = (uint8_t *)heap_caps_aligned_calloc(128, 1, kFieldBytes, MALLOC_CAP_SPIRAM);
  }
  for (int i = 0; i < kOutBufs; i++) {
    // Cache-line aligned: the PPA writes these by DMA. Zeroed: the rows and columns the picture leaves stay black.
    s_out[i] = (uint16_t *)heap_caps_aligned_calloc(128, 1, kOutBytes, MALLOC_CAP_SPIRAM);
  }
  s_rgb = (uint16_t *)heap_caps_aligned_calloc(128, 1, (size_t)kCropW * kPalH * 2, MALLOC_CAP_SPIRAM);
  for (int i = 0; i < kCapBufs; i++) {
    if (!s_field[i]) return false;
  }
  for (int i = 0; i < kOutBufs; i++) {
    if (!s_out[i]) return false;
  }
  if (!s_rgb) return false;
  build_tables();
  ppa_client_config_t pcfg = {};
  pcfg.oper_type = PPA_OPERATION_SRM;
  pcfg.max_pending_trans_num = 1;
  if (ppa_register_client(&pcfg, &s_ppa) != ESP_OK) {
    s_ppa = nullptr;
    Serial.println("[camera] PPA unavailable: scaling on the CPU");
  }
  xTaskCreatePinnedToCore(convert_task, "camera", 4096, nullptr, 5, &s_conv_task, 0);

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
  Serial.printf("[camera] TVP5150 at 0x%02X, DVP ready (%u KB per field ×%d)\n", s_addr, (unsigned)(kFieldBytes / 1024),
                kCapBufs);
  return true;
}

void capture_run(bool on) {
  if (!s_cam || on == s_running) {
    return;
  }
  // Stop capturing while the view is hidden: PSRAM bandwidth is what the gauges live on. Enable / disable as well:
  // the IDF DVP driver only turns the camera stream on in enable, and stop turns it off, so start alone never resumes.
  if (on) {
    esp_cam_ctlr_enable(s_cam);
    esp_cam_ctlr_start(s_cam);
    s_field_seen = s_fields;
    s_field_ms = millis();
  } else {
    esp_cam_ctlr_stop(s_cam);
    esp_cam_ctlr_disable(s_cam);
  }
  s_running = on;
}

// Every few seconds the capture stops delivering fields, with clean VSYNC timing up to the stall and whether or not
// the converter runs (bench, 2026-10-27); only the full reset in enable brings it back.
// Ruled out on 2026-11-06 (about one stall a second then): decoder lock loss (it stays locked through them), missed
// VSYNC edges (two-field buffers: no gap over 25 ms) and the driver's cache sync at the buffer switch (a one-line
// sync left the rate unchanged). Left: PCLK glitches on the bench's jumper wires, or the DVP driver / hardware. Restart after 50 ms (three
// fields) so a stall is a blip rather than a frozen picture.
void capture_watchdog(uint32_t now) {
  if (!s_running) {
    return;
  }
  if (s_fields != s_field_seen) {
    s_field_seen = s_fields;
    s_field_ms = now;
  } else if (now - s_field_ms > 50u) {
    s_field_us = 0;  // the restart gap is not a field
    capture_run(false);
    capture_run(true);
    s_restarts++;
  }
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

  static lv_point_t pts[kNumGuides][2];
  for (int i = 0; i < kNumGuides; i++) {
    const Guide &g = kGuides[i];
    pts[i][0] = {(lv_coord_t)(x0 + guide_x(g.ax)), (lv_coord_t)(y0 + guide_y(g.ay))};
    pts[i][1] = {(lv_coord_t)(x0 + guide_x(g.bx)), (lv_coord_t)(y0 + guide_y(g.by))};
    line(s_layer, pts[i], 2, g.color);
  }

  s_msg = lv_label_create(s_layer);
  lv_obj_set_style_text_font(s_msg, &elc_sm_28, 0);
  lv_obj_set_style_text_color(s_msg, lv_color_hex(kMsgText), 0);
  lv_obj_set_style_text_align(s_msg, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_bg_color(s_msg, lv_color_black(), 0);  // over the picture now: a dark pill keeps it legible
  lv_obj_set_style_bg_opa(s_msg, LV_OPA_50, 0);
  lv_obj_set_style_radius(s_msg, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_pad_hor(s_msg, kMsgPadH, 0);
  lv_obj_set_style_pad_ver(s_msg, kMsgPadV, 0);
  lv_obj_align(s_msg, LV_ALIGN_TOP_MID, 0, kMsgTop);
  lv_label_set_text(s_msg, "REAR CAMERA");
}

// ---------------------------------------------------------------- direct-mode overlay (drawn once)
// "Over" with straight alpha onto an ARGB8888 canvas the size of the screen.
void ov_put(uint32_t *cv, int x, int y, uint32_t rgb, int a) {
  if (x < 0 || y < 0 || x >= kOutW || y >= kOutH || a <= 0) return;
  if (a > 255) a = 255;
  uint32_t &d = cv[(size_t)y * kOutW + x];
  const int da = (int)(d >> 24), oa = a + da * (255 - a) / 255;
  if (!oa) return;
  uint32_t out = (uint32_t)oa << 24;
  for (int sh = 0; sh <= 16; sh += 8) {
    const int s = (int)((rgb >> sh) & 255), dc = (int)((d >> sh) & 255);
    out |= (uint32_t)((s * a + dc * da * (255 - a) / 255) / oa) << sh;
  }
  d = out;
}

// A thick segment with round ends (a capsule) of radius r between pixel-centre points, anti-aliased.
void ov_capsule(uint32_t *cv, float ax, float ay, float bx, float by, float r, uint32_t rgb, int alpha) {
  const int xa = (int)floorf(fminf(ax, bx) - r - 1), xb = (int)ceilf(fmaxf(ax, bx) + r + 1);
  const int ya = (int)floorf(fminf(ay, by) - r - 1), yb = (int)ceilf(fmaxf(ay, by) + r + 1);
  const float dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
  for (int y = ya; y <= yb; y++) {
    for (int x = xa; x <= xb; x++) {
      const float px = x + 0.5f, py = y + 0.5f;
      float t = len2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0.0f;
      t = t < 0 ? 0 : (t > 1 ? 1 : t);
      const float ex = ax + t * dx - px, ey = ay + t * dy - py;
      float cov = r + 0.5f - sqrtf(ex * ex + ey * ey);
      if (cov <= 0) continue;
      if (cov > 1) cov = 1;
      ov_put(cv, x, y, rgb, (int)(alpha * cov + 0.5f));
    }
  }
}

int text_width(const lv_font_t *font, const char *txt) {
  int w = 0;
  for (const char *c = txt; *c; c++) {
    lv_font_glyph_dsc_t g;
    if (lv_font_get_glyph_dsc(font, &g, (uint8_t)c[0], (uint8_t)c[1])) w += g.adv_w;
  }
  return w;
}

// ASCII text with its line's top at (x, y), placed as LVGL places glyphs; 4 bpp fonts (packed rows, high nibble first).
void ov_text(uint32_t *cv, const lv_font_t *font, int x, int y, const char *txt, uint32_t rgb) {
  for (const char *c = txt; *c; c++) {
    lv_font_glyph_dsc_t g;
    if (!lv_font_get_glyph_dsc(font, &g, (uint8_t)c[0], (uint8_t)c[1])) continue;
    const uint8_t *bmp = lv_font_get_glyph_bitmap(font, (uint8_t)c[0]);
    if (bmp && g.bpp == 4) {
      const int gx = x + g.ofs_x, gy = y + (font->line_height - font->base_line) - g.box_h - g.ofs_y;
      for (int j = 0; j < g.box_h; j++) {
        for (int i = 0; i < g.box_w; i++) {
          const uint32_t bit = (uint32_t)(j * g.box_w + i) * 4;
          const int v = (bmp[bit >> 3] >> (4 - (bit & 7))) & 0x0F;
          ov_put(cv, gx + i, gy + j, rgb, v * 17);
        }
      }
    }
    x += g.adv_w;
  }
}

/** Draw the guides and the "REAR CAMERA" pill as LVGL does, then keep only the pixels they cover, as runs. */
void build_overlay() {
  auto *cv = (uint32_t *)heap_caps_calloc((size_t)kOutW * kOutH, 4, MALLOC_CAP_SPIRAM);
  if (!cv) return;
  const char *txt = "REAR CAMERA";
  const lv_font_t *font = &elc_sm_28;
  const int pw = text_width(font, txt) + 2 * kMsgPadH, ph = font->line_height + 2 * kMsgPadV;
  const int px = (kOutW - pw) / 2, py = kMsgTop;
  const float pr = ph / 2.0f;
  ov_capsule(cv, px + pr, py + pr, px + pw - pr, py + pr, pr - 0.5f, 0x000000, LV_OPA_50);
  ov_text(cv, font, px + kMsgPadH, py + kMsgPadV, txt, kMsgText);
  for (const Guide &g : kGuides) {
    ov_capsule(cv, guide_x(g.ax) + 0.5f, guide_y(g.ay) + 0.5f, guide_x(g.bx) + 0.5f, guide_y(g.by) + 0.5f,
               kGuideWidth / 2.0f - 0.5f, g.color, 255);
  }
  int runs = 0, pixels = 0;
  for (int y = 0; y < kOutH; y++) {
    for (int x = 0; x < kOutW; x++) {
      if (cv[(size_t)y * kOutW + x] >> 24) {
        pixels++;
        if (x == 0 || !(cv[(size_t)y * kOutW + x - 1] >> 24)) runs++;
      }
    }
  }
  s_ov_run = (OvRun *)heap_caps_calloc((size_t)runs, sizeof(OvRun), MALLOC_CAP_SPIRAM);
  s_ov_col = (uint16_t *)heap_caps_calloc((size_t)pixels, 2, MALLOC_CAP_SPIRAM);
  s_ov_alpha = (uint8_t *)heap_caps_calloc((size_t)pixels, 1, MALLOC_CAP_SPIRAM);
  if (!s_ov_run || !s_ov_col || !s_ov_alpha) {
    heap_caps_free(cv);
    return;  // no overlay: the picture alone
  }
  int nr = 0, np = 0;
  for (int y = 0; y < kOutH; y++) {
    for (int x = 0; x < kOutW; x++) {
      const uint32_t v = cv[(size_t)y * kOutW + x];
      if (!(v >> 24)) continue;
      if (x == 0 || !(cv[(size_t)y * kOutW + x - 1] >> 24)) s_ov_run[nr++] = {(uint16_t)y, (uint16_t)x, 0, (uint32_t)np};
      s_ov_run[nr - 1].n++;
      s_ov_col[np] = (uint16_t)(((v >> 8) & 0xF800) | ((v >> 5) & 0x07E0) | ((v >> 3) & 0x001F));
      s_ov_alpha[np] = (uint8_t)(v >> 24);
      np++;
    }
  }
  s_ov_runs = nr;
  heap_caps_free(cv);
}

void set_msg(const char *m) {
  if (strcmp(lv_label_get_text(s_msg), m) != 0) {
    lv_label_set_text(s_msg, m);
  }
}

}  // namespace

// ---------------------------------------------------------------- public

void center_camera_begin() {
  if (kReverseGpio >= 0) pinMode(kReverseGpio, INPUT_PULLUP);
  tvp_reset();
  if (tvp_probe()) {
    tvp_init();
    capture_create();
  }
  build_ui();
  build_overlay();
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
  const bool raw = kReverseGpio >= 0 && digitalRead(kReverseGpio) == LOW;
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
    if (!want && s_direct_on) {
      p4_screen_set_direct(false);  // waits out a direct frame; LVGL redraws the whole screen
      s_direct_on = false;
    }
    s_shown = want;
    if (want) {
      lv_obj_clear_flag(s_layer, LV_OBJ_FLAG_HIDDEN);
      lv_obj_move_foreground(s_layer);
    } else {
      lv_obj_add_flag(s_layer, LV_OBJ_FLAG_HIDDEN);
    }
    capture_run(want);
  }
  capture_watchdog(now);
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
  // The camera task (other core) can stamp a frame after `now` was read: a negative age is a fresh frame, not ~49 days.
  const uint32_t last_frame = s_last_frame_ms;
  const uint32_t frame_age = (int32_t)(now - last_frame) < 0 ? 0u : now - last_frame;
  const bool live = last_frame && frame_age < 500u;
  // Live picture: hand the screen to the camera task (direct mode). Signal gone for 1.5 s: back to LVGL, which shows the
  // layer without the stale picture and with the message. Shorter gaps (the decoder dropping lock for a moment, a
  // capture restart) hold the last frame instead of flashing the message.
  const bool stale = !last_frame || frame_age > 1500u;
  const bool direct = s_direct_on ? !stale : live;
  if (direct != s_direct_on) {
    if (!direct) {
      p4_screen_set_direct(false);
      s_direct_on = false;
      lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);
    } else if (p4_screen_set_direct(true)) {
      s_direct_on = true;
      lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);  // so leaving direct mode never shows an old LVGL frame
    }
  }
  if (!s_addr) {
    set_msg("NO VIDEO MODULE");
  } else if (!live) {
    set_msg("NO CAMERA SIGNAL");
  } else {
    set_msg("REAR CAMERA");
  }
  if (!live && !lv_obj_has_flag(s_img, LV_OBJ_FLAG_HIDDEN) && stale) {
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
           "capture %s: %lu fields/s, %lu frames/s, %lu active lines, convert %.1f ms, %lu restarts, %lu partial fields dropped, vsync%s pclk%s, "
           "scaling %s (find lines %.1f, colour %.1f, scale %.1f ms), screen %s (%lu frames; overlay + writeback %.1f, "
           "flip %.1f ms)",
           s_addr ? "TVP5150 @0x" : "not found", s_addr ? (s_addr == 0x5C ? "5C" : "5D") : "",
           !st ? "?" : ((s1 & 0x06) == 0x06 ? "locked" : "no lock"), s1, s5, s_rev ? "ON" : "off",
           s_mode == CameraMode::On ? "on" : (s_mode == CameraMode::Off ? "off" : "auto"), s_shown ? "shown" : "hidden",
           s_running ? "running" : "stopped", (unsigned long)s_fields_ps, (unsigned long)s_frames_ps,
           (unsigned long)s_last_lines, (double)s_convert_us / 1000.0, (unsigned long)s_restarts, (unsigned long)s_partial, s_vsync_inv ? " inverted" : "",
           s_pclk_inv ? " inverted" : "", s_ppa ? "PPA" : "CPU", s_scan_us / 1000.0, s_csc_us / 1000.0, s_scale_us / 1000.0,
           s_direct_on ? "direct" : "LVGL", (unsigned long)s_direct_frames, s_finish_us / 1000.0, s_show_us / 1000.0);
}

void center_camera_field_timing(char *out, size_t n) {
  snprintf(out, n, "field gaps: min %lu us, max %lu us, %lu under 15 ms, %lu over 25 ms (missed VSYNC) (NTSC 16683, PAL 20000)",
           (unsigned long)(s_gap_min == UINT32_MAX ? 0 : s_gap_min), (unsigned long)s_gap_max,
           (unsigned long)s_gap_short, (unsigned long)s_gap_long);
  s_gap_min = UINT32_MAX;
  s_gap_max = 0;
  s_gap_short = 0;
  s_gap_long = 0;
}

void center_camera_pin_edges(char *out, size_t n) {
  // Polled, not counted in hardware: PCLK (~27 MHz) is far faster than the loop, so its count only says "toggling";
  // VSYNC (~60 Hz) is slow enough to count properly (about 24 edges in 200 ms).
  constexpr uint32_t kWindowUs = 200000;
  uint32_t samples = 0, pclk_edges = 0, vsync_edges = 0, pclk_high = 0, vsync_high = 0;
  int pclk = gpio_get_level(kPclk), vsync = gpio_get_level(kVsync);
  const uint32_t t0 = micros();
  while (micros() - t0 < kWindowUs) {
    const int p = gpio_get_level(kPclk), v = gpio_get_level(kVsync);
    pclk_edges += p != pclk;
    vsync_edges += v != vsync;
    pclk_high += p;
    vsync_high += v;
    pclk = p;
    vsync = v;
    samples++;
  }
  snprintf(out, n, "pins over %lu ms (%lu samples): PCLK GPIO%d %lu edges, high %lu%%; VSYNC GPIO%d %lu edges, high %lu%%%s",
           (unsigned long)(kWindowUs / 1000), (unsigned long)samples, (int)kPclk, (unsigned long)pclk_edges,
           (unsigned long)(samples ? pclk_high * 100 / samples : 0), (int)kVsync, (unsigned long)vsync_edges,
           (unsigned long)(samples ? vsync_high * 100 / samples : 0),
           s_cam ? "" : " (capture not set up: pins may not be inputs yet)");
}

bool center_camera_bit_check(char *out, size_t n) {
  if (!s_cam || !s_running) {
    snprintf(out, n, "cam bits: capture is stopped (`cam on` first)\n");
    return false;
  }
  __atomic_store_n(&s_bits_state, 1, __ATOMIC_RELEASE);
  const uint32_t t0 = millis();
  while (__atomic_load_n(&s_bits_state, __ATOMIC_ACQUIRE) != 2 && millis() - t0 < 500u) delay(5);
  if (__atomic_load_n(&s_bits_state, __ATOMIC_ACQUIRE) != 2) {
    __atomic_store_n(&s_bits_state, 0, __ATOMIC_RELEASE);
    snprintf(out, n, "cam bits: no field arrived in 500 ms\n");
    return false;
  }
  const BitReport r = s_bits;
  __atomic_store_n(&s_bits_state, 0, __ATOMIC_RELEASE);
  if (!r.bytes) {
    snprintf(out, n, "cam bits: the field was empty\n");
    return false;
  }
  size_t k = 0;
  auto put = [&](const char *fmt, auto... a) {
    if (k < n) {
      const int w = snprintf(out + k, n - k, fmt, a...);
      if (w > 0) k += (size_t)w;
    }
  };
  put("cam bits: %lu bytes, timing codes %lu valid / %lu bad (a clean field: about 500 NTSC, 600 PAL), %lu active-line "
      "SAVs (~240 / ~288), %lu blanking bytes checked\n",
      (unsigned long)r.bytes, (unsigned long)r.codes_ok, (unsigned long)r.codes_bad, (unsigned long)r.active_sav,
      (unsigned long)r.blank_bytes);
  put("cam bits: bit GPIO  ones%%  changes%%  codes wrong  blanking wrong: read 1 / read 0\n");
  const double blank = r.blank_bytes ? (double)r.blank_bytes : 1.0;
  // Picture data keeps every line busy: a bit that changes far less often than the median of the others is stuck or
  // weakly driven, even where blanking cannot show it (blanking only exercises bits 7 and 4). Its share of ones alone
  // is no test: the top bits follow how bright the picture is.
  uint32_t sorted[8];
  memcpy(sorted, r.toggles, sizeof(sorted));
  for (int a = 1; a < 8; a++) {
    for (int b = a; b > 0 && sorted[b] < sorted[b - 1]; b--) {
      const uint32_t t = sorted[b];
      sorted[b] = sorted[b - 1];
      sorted[b - 1] = t;
    }
  }
  const uint32_t median = sorted[4];
  const uint32_t codes = r.codes_ok + r.codes_bad;
  char suspects[200] = "";
  size_t sk = 0;
  for (int bit = 0; bit < 8; bit++) {
    const uint32_t err = r.err_hi[bit] + r.err_lo[bit];
    put("cam bits:  D%d  %2d   %5.1f   %5.1f    %6lu        %lu / %lu (%.2f%%)\n", bit, (int)kData[bit],
        100.0 * r.ones[bit] / r.bytes, 100.0 * r.toggles[bit] / r.bytes, (unsigned long)r.code_err[bit],
        (unsigned long)r.err_hi[bit], (unsigned long)r.err_lo[bit], 100.0 * err / blank);
    const char *why = nullptr;
    if (r.ones[bit] == 0) why = "stuck low";
    else if (r.ones[bit] == r.bytes) why = "stuck high";
    else if (r.toggles[bit] == 0) why = "never changes";
    else if (r.toggles[bit] * 3u < median)
      why = r.ones[bit] * 2u < r.bytes ? "mostly low (open, weak or shorted to GND?)" : "mostly high (shorted to 3V3?)";
    else if (codes && r.code_err[bit] * 20u > codes) why = "wrong in timing codes";  // over 5 % of codes
    else if (r.blank_bytes && err * 200u > r.blank_bytes) why = "wrong in blanking";  // over 0.5 %
    if (why && sk < sizeof(suspects)) {
      const int w = snprintf(suspects + sk, sizeof(suspects) - sk, "%sD%d (GPIO%d) %s", sk ? ", " : "", bit,
                             (int)kData[bit], why);
      if (w > 0) sk += (size_t)w;
    }
  }
  put("cam bits: suspect: %s\n", sk ? suspects : "none in blanking (if the picture is still bad: grounding, lead length)");
  if (r.chroma_n) {
    put("cam bits: colour: Cb %.1f, Cr %.1f from neutral on average, %.1f%% of samples beyond 16 (0 and 0%%: no colour "
        "in the signal, e.g. a camera in its black-and-white low-light mode)\n",
        (double)r.cb_dev / r.chroma_n, (double)r.cr_dev / r.chroma_n, 100.0 * r.chroma_strong / r.chroma_n);
  }
  put("cam bits: colour by eighth of the line (Cb mean / mean |Cb| / Cr mean):");
  for (int b = 0; b < 8; b++) {
    const double nb = r.bin_n[b] ? (double)r.bin_n[b] : 1.0;
    put(" %+.0f/%.0f/%+.0f", r.cb_sum[b] / nb, r.cb_abs[b] / nb, r.cr_sum[b] / nb);
  }
  put("\n");
  put("cam bits: first EAV:");
  for (int j = 0; j < r.sample_len; j++) put(" %02X", r.sample[j]);
  put(r.sample_len ? " (should be FF 00 00 XY then 80 10 80 10 …)\n" : " none found\n");
  return true;
}

void center_camera_toggle_vsync_invert() {
  s_vsync_inv = !s_vsync_inv;
  esp_rom_gpio_connect_in_signal(kVsync, CAM_V_SYNC_PAD_IN_IDX, s_vsync_inv);
}

void center_camera_toggle_pclk_invert() {
  s_pclk_inv = !s_pclk_inv;
  esp_rom_gpio_connect_in_signal(kPclk, CAM_PCLK_PAD_IN_IDX, s_pclk_inv);
}

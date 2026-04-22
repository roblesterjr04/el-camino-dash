#include <display_setup.h>

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_log.h>

#if defined(ARDUINO_ARCH_ESP32) && defined(CONFIG_IDF_TARGET_ESP32P4)
#include <esp32-hal-ledc.h>
#include <esp_lcd_panel_io.h>
// Set by patched GFX Arduino_ESP32DSIPanel.cpp after esp_lcd_new_panel_io_dbi(); used for SH8601 DCS brightness.
esp_lcd_panel_io_handle_t g_elc_dsi_panel_io = nullptr;
#endif

#if (defined(TSPS_USE_JD9365) && TSPS_USE_JD9365) || (defined(TSPS_WAVESHARE_3C4) && TSPS_WAVESHARE_3C4)
#include <Wire.h>
#endif
#if defined(TSPS_USE_JD9365) && TSPS_USE_JD9365
#include "waveshare_jd9365_720_init.h"
#endif
#if defined(TSPS_WAVESHARE_3C4) && TSPS_WAVESHARE_3C4
#include "waveshare_3inch4_dsi_800_init.h"
#endif

// Set -DTSPS_USE_JD9365=1 for Waveshare 4" 720×720 (JD9365). Set -DTSPS_WAVESHARE_3C4=1 for
// ESP32-P4-WIFI6-Touch-LCD-3.4C (800×800 SH8601). Wrong profile → black MIPI panel.
#if defined(TSPS_WAVESHARE_3C4) && TSPS_WAVESHARE_3C4
#define TSPS_LCD_W 800
#define TSPS_LCD_H 800
#define TSPS_DSI_HPW 20
#define TSPS_DSI_HBP 20
#define TSPS_DSI_HFP 40
#define TSPS_DSI_VPW 4
#define TSPS_DSI_VBP 12
#define TSPS_DSI_VFP 24
#define TSPS_DSI_DPI_HZ 80000000L
#define TSPS_DSI_LANE_MBPS 1500
#elif defined(TSPS_USE_JD9365) && TSPS_USE_JD9365
#define TSPS_LCD_W 720
#define TSPS_LCD_H 720
// From Waveshare esp_lcd_jd9365: JD9365_720_720_PANEL_60HZ_DPI_CONFIG (RGB565)
#define TSPS_DSI_HPW 20
#define TSPS_DSI_HBP 20
#define TSPS_DSI_HFP 40
#define TSPS_DSI_VPW 4
#define TSPS_DSI_VBP 12
#define TSPS_DSI_VFP 24
#define TSPS_DSI_DPI_HZ 80000000L
#define TSPS_DSI_LANE_MBPS 1200
#elif defined(TSPS_PANEL_1024X600) && TSPS_PANEL_1024X600
#define TSPS_LCD_W 1024
#define TSPS_LCD_H 600
#define TSPS_DSI_HPW 40
#define TSPS_DSI_HBP 160
#define TSPS_DSI_HFP 160
#define TSPS_DSI_VPW 10
#define TSPS_DSI_VBP 23
#define TSPS_DSI_VFP 12
#define TSPS_DSI_DPI_HZ 48000000L
#define TSPS_DSI_LANE_MBPS 750
#else
#define TSPS_LCD_W 720
#define TSPS_LCD_H 720
// Espressif EV board: square 720×720 JD9165 module (not the stock 7" 1024×600 kit).
#define TSPS_DSI_HPW 8
#define TSPS_DSI_HBP 64
#define TSPS_DSI_HFP 64
#define TSPS_DSI_VPW 4
#define TSPS_DSI_VBP 16
#define TSPS_DSI_VFP 16
#define TSPS_DSI_DPI_HZ 56000000L
#define TSPS_DSI_LANE_MBPS 750
#endif

#if defined(TSPS_USE_JD9365) && TSPS_USE_JD9365
#ifndef TSPS_LCD_RST
#define TSPS_LCD_RST GFX_NOT_DEFINED
#endif
#else
#ifndef TSPS_LCD_RST
#define TSPS_LCD_RST 27
#endif
#endif

// Waveshare round MIPI kits: PMIC at I2C 0x45 (SDA=7/SCL=8) before DSI; reg 0x96 often drives backlight.
#if (defined(TSPS_USE_JD9365) && TSPS_USE_JD9365) || (defined(TSPS_WAVESHARE_3C4) && TSPS_WAVESHARE_3C4)
#ifndef TSPS_WAVESHARE_PMIC_I2C_ADDR
#define TSPS_WAVESHARE_PMIC_I2C_ADDR 0x45
#endif
#ifndef TSPS_WAVESHARE_PMIC_SDA
#define TSPS_WAVESHARE_PMIC_SDA 7
#endif
#ifndef TSPS_WAVESHARE_PMIC_SCL
#define TSPS_WAVESHARE_PMIC_SCL 8
#endif
#endif

// --- Backlight (TSPS_BACKLIGHT_*) vs. ambient illumination (ELC_ILLUMINATION_*) ---
// TSPS_BACKLIGHT_*: boot + driver path — GPIO PWM/digital (TSPS_BACKLIGHT_PIN ≥ 0), or PMIC I2C 0x45/0x96 (+ SH8601
//   DCS 0x51 on 3.4C) when TSPS_BACKLIGHT_PIN is -1 on Waveshare kits. Used by tsps_init_backlight / tsps_backlight_set_level.
// ELC_ILLUMINATION_*: vehicle digital IN on ELC_ILLUMINATION_GPIO (default 24); poll calls tsps_backlight_set_level for
//   DIM/FULL. Must not share a GPIO with TSPS_BACKLIGHT_PIN or TSPS_LCD_RST. TSPS_BACKLIGHT_LEVEL is initial brightness only
//   until the first illum poll updates the level.
// Most Waveshare / Espressif MIPI kits need the BL pin driven (3.4C round kit: PWM on GPIO 26 per schematic); without it the
// panel looks dead (black). If BL is hardwired / PMIC-only on your variant, set TSPS_BACKLIGHT_PIN=-1 in platformio.ini.
#ifndef TSPS_BACKLIGHT_PIN
#define TSPS_BACKLIGHT_PIN 26
#endif
#ifndef TSPS_BACKLIGHT_USE_PWM
#define TSPS_BACKLIGHT_USE_PWM 1
#endif
#ifndef TSPS_BACKLIGHT_ACTIVE_LOW
#define TSPS_BACKLIGHT_ACTIVE_LOW 0
#endif
// Backlight intensity 0–255 (PWM duty / PMIC reg 0x96). Override with -DTSPS_BACKLIGHT_LEVEL=… in platformio.ini.
#ifndef TSPS_BACKLIGHT_LEVEL
#define TSPS_BACKLIGHT_LEVEL 51
#endif

#if defined(CONFIG_IDF_TARGET_ESP32P4)
// Ambient illum (defaults here so boot PWM + tsps_backlight_set_level can use them). Override in platformio.ini.
#ifndef ELC_ILLUMINATION_GPIO
#define ELC_ILLUMINATION_GPIO 24
#endif
#ifndef ELC_ILLUMINATION_DIM_LEVEL
#define ELC_ILLUMINATION_DIM_LEVEL 100
#endif
#ifndef ELC_ILLUMINATION_FULL_LEVEL
#define ELC_ILLUMINATION_FULL_LEVEL 255
#endif
#ifndef ELC_ILLUMINATION_LOW_IS_DIM
#define ELC_ILLUMINATION_LOW_IS_DIM 1
#endif
#endif

#if TSPS_BACKLIGHT_PIN >= 0
static bool s_tsps_bl_ledc = false;
#if defined(CONFIG_IDF_TARGET_ESP32P4)
// logical: 0 = dark, 255 = bright. Many BL FETs are active-low (GPIO high = LED off).
static uint8_t tsps_backlight_pwm_duty(uint8_t logical) {
#if TSPS_BACKLIGHT_ACTIVE_LOW
  return (uint8_t)(255u - (unsigned)logical);
#else
  return logical;
#endif
}
#endif
#endif

#if (defined(TSPS_USE_JD9365) && TSPS_USE_JD9365) || (defined(TSPS_WAVESHARE_3C4) && TSPS_WAVESHARE_3C4)
static void tsps_waveshare_p4_mipi_power_on() {
  Wire.begin(TSPS_WAVESHARE_PMIC_SDA, TSPS_WAVESHARE_PMIC_SCL);
  Wire.setClock(100000);
  auto wr = [](uint8_t reg, uint8_t val) {
    Wire.beginTransmission(TSPS_WAVESHARE_PMIC_I2C_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission();
  };
  wr(0x95, 0x11);
  wr(0x95, 0x17);
  wr(0x96, 0x00);
  delay(100);
  // With TSPS_BACKLIGHT_PIN ≥ 0 (e.g. Waveshare 3.4C BL PWM on GPIO 26), max PMIC current; dimming is LEDC on that pin.
  wr(0x96, (uint8_t)(TSPS_BACKLIGHT_PIN < 0 ? TSPS_BACKLIGHT_LEVEL : 255));
  for (int i = 0; i < 10; i++) {
    delay(100);
    yield();
  }
  Serial.println("TSPS: Waveshare display PMIC (I2C 0x45) power sequence done");
}
#endif

static const char *kTspsDispTag = "tsps-disp";

#if defined(CONFIG_IDF_TARGET_ESP32P4) && defined(TSPS_WAVESHARE_3C4) && TSPS_WAVESHARE_3C4
// SH8601: init table sets 0x51 under user extension bank 0xE0=0x02 (see waveshare_3inch4_dsi_800_init.cpp).
static void tsps_sh8601_brightness_via_dsi(uint8_t level) {
  if (g_elc_dsi_panel_io == nullptr) {
    return;
  }
  const uint8_t bank_user = 0x02;
  (void)esp_lcd_panel_io_tx_param(g_elc_dsi_panel_io, 0xE0, &bank_user, 1);
  (void)esp_lcd_panel_io_tx_param(g_elc_dsi_panel_io, 0x51, &level, 1);
  const uint8_t bank_idle = 0x00;
  (void)esp_lcd_panel_io_tx_param(g_elc_dsi_panel_io, 0xE0, &bank_idle, 1);
}
#endif

static void tsps_init_backlight() {
#if ((defined(TSPS_USE_JD9365) && TSPS_USE_JD9365) || (defined(TSPS_WAVESHARE_3C4) && TSPS_WAVESHARE_3C4)) && TSPS_BACKLIGHT_PIN < 0
  Wire.beginTransmission(TSPS_WAVESHARE_PMIC_I2C_ADDR);
  Wire.write(0x96);
  Wire.write((uint8_t)TSPS_BACKLIGHT_LEVEL);
  if (Wire.endTransmission() != 0) {
    Serial.println("TSPS: WARN I2C backlight (0x45/0x96) failed");
  } else {
    Serial.println("TSPS: backlight via Waveshare PMIC I2C");
  }
#if defined(TSPS_WAVESHARE_3C4) && TSPS_WAVESHARE_3C4
  tsps_sh8601_brightness_via_dsi((uint8_t)TSPS_BACKLIGHT_LEVEL);
#endif
#elif TSPS_BACKLIGHT_PIN >= 0
  const int pin = TSPS_BACKLIGHT_PIN;
#if TSPS_BACKLIGHT_USE_PWM && defined(CONFIG_IDF_TARGET_ESP32P4)
  if (ledcAttach(pin, 20000, 8)) {
    s_tsps_bl_ledc = true;
#if defined(CONFIG_IDF_TARGET_ESP32P4) && ELC_ILLUMINATION_GPIO >= 0
    // Safe default until elc_illumination_gpio_poll(): visible dim, not logical "full" that is off on active-low BL.
    ledcWrite(pin, tsps_backlight_pwm_duty((uint8_t)ELC_ILLUMINATION_DIM_LEVEL));
#else
    ledcWrite(pin, tsps_backlight_pwm_duty((uint8_t)TSPS_BACKLIGHT_LEVEL));
#endif
    Serial.printf("TSPS: backlight PWM GPIO %d active_%s\n", pin, TSPS_BACKLIGHT_ACTIVE_LOW ? "low" : "high");
    return;
  }
#endif
  pinMode(pin, OUTPUT);
#if TSPS_BACKLIGHT_ACTIVE_LOW
  digitalWrite(pin, LOW);
#else
  digitalWrite(pin, HIGH);
#endif
  Serial.printf("TSPS: backlight GPIO %d digital on\n", pin);
#else
  Serial.println("TSPS: backlight disabled (TSPS_BACKLIGHT_PIN=-1)");
#endif
}

void tsps_backlight_set_level(uint8_t level) {
#if !defined(CONFIG_IDF_TARGET_ESP32P4)
  (void)level;
#else
#if ((defined(TSPS_USE_JD9365) && TSPS_USE_JD9365) || (defined(TSPS_WAVESHARE_3C4) && TSPS_WAVESHARE_3C4)) && TSPS_BACKLIGHT_PIN < 0
  // Default `Wire` is also used by touch/audio on the display bus; re-select PMIC pins before each write.
  Wire.begin(TSPS_WAVESHARE_PMIC_SDA, TSPS_WAVESHARE_PMIC_SCL);
  Wire.setClock(100000);
  Wire.beginTransmission(TSPS_WAVESHARE_PMIC_I2C_ADDR);
  Wire.write(0x96);
  Wire.write(level);
  (void)Wire.endTransmission();
#if defined(TSPS_WAVESHARE_3C4) && TSPS_WAVESHARE_3C4
  tsps_sh8601_brightness_via_dsi(level);
#endif
#elif TSPS_BACKLIGHT_PIN >= 0
  const int pin = TSPS_BACKLIGHT_PIN;
#if TSPS_BACKLIGHT_USE_PWM && defined(CONFIG_IDF_TARGET_ESP32P4)
  if (s_tsps_bl_ledc) {
    ledcWrite(pin, tsps_backlight_pwm_duty(level));
    return;
  }
#endif
#if TSPS_BACKLIGHT_ACTIVE_LOW
  digitalWrite(pin, level >= 128 ? LOW : HIGH);
#else
  digitalWrite(pin, level >= 128 ? HIGH : LOW);
#endif
#else
  (void)level;
#endif
#endif
}

#if defined(CONFIG_IDF_TARGET_ESP32P4)
// ELC_ILLUMINATION_* defaults are near TSPS_BACKLIGHT_* above.

#if TSPS_BACKLIGHT_PIN >= 0 && ELC_ILLUMINATION_GPIO >= 0 && TSPS_BACKLIGHT_PIN == ELC_ILLUMINATION_GPIO
#error TSPS_BACKLIGHT_PIN and ELC_ILLUMINATION_GPIO must be different GPIOs (backlight out vs. illum input)
#endif
#if ELC_ILLUMINATION_GPIO >= 0 && TSPS_LCD_RST != GFX_NOT_DEFINED && TSPS_LCD_RST >= 0 && ELC_ILLUMINATION_GPIO == TSPS_LCD_RST
#error ELC_ILLUMINATION_GPIO must not equal TSPS_LCD_RST (reset output vs. illum input)
#endif

void elc_illumination_gpio_poll(void) {
#if ELC_ILLUMINATION_GPIO < 0
  return;
#else
  static bool s_pin_inited = false;
  if (!s_pin_inited) {
    pinMode(ELC_ILLUMINATION_GPIO, INPUT_PULLUP);
    s_pin_inited = true;
  }
  const bool high = digitalRead(ELC_ILLUMINATION_GPIO) == HIGH;
  // LOW_IS_DIM 1: GPIO low = night → dim. Set -DELC_ILLUMINATION_LOW_IS_DIM=0 if your harness is inverted.
  const uint8_t want =
#if ELC_ILLUMINATION_LOW_IS_DIM
      high ? (uint8_t)ELC_ILLUMINATION_FULL_LEVEL : (uint8_t)ELC_ILLUMINATION_DIM_LEVEL;
#else
      high ? (uint8_t)ELC_ILLUMINATION_DIM_LEVEL : (uint8_t)ELC_ILLUMINATION_FULL_LEVEL;
#endif
  static uint16_t s_last_level = 0xFFFFu;
  static uint32_t s_last_apply_ms = 0;
  const uint32_t now = millis();
  if (s_last_level != 0xFFFFu && want == (uint8_t)s_last_level) {
    return;
  }
#ifndef ELC_ILLUMINATION_DEBOUNCE_MS
#define ELC_ILLUMINATION_DEBOUNCE_MS 25u
#endif
  if (s_last_level != 0xFFFFu && (now - s_last_apply_ms) < ELC_ILLUMINATION_DEBOUNCE_MS) {
    return;
  }
  s_last_apply_ms = now;
  s_last_level = want;
  tsps_backlight_set_level(want);
#endif
}
#else
void elc_illumination_gpio_poll(void) {}
#endif

Arduino_GFX *create_tsps_display() {
#if !defined(CONFIG_IDF_TARGET_ESP32P4)
  return nullptr;
#else
  ESP_LOGI(kTspsDispTag, "create_tsps_display() start (%dx%d)", (int)TSPS_LCD_W, (int)TSPS_LCD_H);
#if (defined(TSPS_USE_JD9365) && TSPS_USE_JD9365) || (defined(TSPS_WAVESHARE_3C4) && TSPS_WAVESHARE_3C4)
  ESP_LOGI(kTspsDispTag, "Waveshare path: PMIC then panel init");
  tsps_waveshare_p4_mipi_power_on();
#endif

  Arduino_ESP32DSIPanel *bus = new Arduino_ESP32DSIPanel(
      TSPS_DSI_HPW, TSPS_DSI_HBP, TSPS_DSI_HFP, TSPS_DSI_VPW, TSPS_DSI_VBP, TSPS_DSI_VFP, TSPS_DSI_DPI_HZ,
      TSPS_DSI_LANE_MBPS);

#if defined(TSPS_WAVESHARE_3C4) && TSPS_WAVESHARE_3C4
  size_t ncmds = tsps_waveshare_3inch4_dsi_800_init_ops_len;
  Arduino_DSI_Display *gfx = new Arduino_DSI_Display(
      TSPS_LCD_W, TSPS_LCD_H, bus, 0, true, TSPS_LCD_RST, tsps_waveshare_3inch4_dsi_800_init_ops, ncmds, 0, 0, 0, 0);
#elif defined(TSPS_USE_JD9365) && TSPS_USE_JD9365
  size_t ncmds = tsps_waveshare_jd9365_720_init_ops_len;
  Arduino_DSI_Display *gfx = new Arduino_DSI_Display(
      TSPS_LCD_W, TSPS_LCD_H, bus, 0, true, TSPS_LCD_RST, tsps_waveshare_jd9365_720_init_ops, ncmds, 0, 0, 0, 0);
#else
  size_t ncmds = sizeof(jd9165_init_operations) / sizeof(lcd_init_cmd_t);
  Arduino_DSI_Display *gfx = new Arduino_DSI_Display(
      TSPS_LCD_W, TSPS_LCD_H, bus, 0, true, TSPS_LCD_RST, jd9165_init_operations, ncmds, 0, 0, 0, 0);
#endif

  ESP_LOGI(kTspsDispTag, "gfx->begin() …");
  if (!gfx->begin()) {
    ESP_LOGE(kTspsDispTag, "gfx->begin() failed");
    return nullptr;
  }

  ESP_LOGI(kTspsDispTag, "gfx->begin() OK, backlight");
  tsps_init_backlight();
#if defined(TSPS_WAVESHARE_3C4) && TSPS_WAVESHARE_3C4 && TSPS_BACKLIGHT_PIN < 0
  if (g_elc_dsi_panel_io == nullptr) {
    ESP_LOGW(
        kTspsDispTag,
        "SH8601 DSI panel_io handle is null — run a clean build so pre_extra_script patches GFX "
        "(PMIC-only backlight needs DCS 0x51). Check serial for pre: patched … Arduino_ESP32DSIPanel.cpp");
  }
#endif
  ESP_LOGI(kTspsDispTag, "create_tsps_display() done");
  return gfx;
#endif
}

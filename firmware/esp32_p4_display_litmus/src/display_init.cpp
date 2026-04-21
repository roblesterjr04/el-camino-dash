#include "display_init.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_log.h>

#if defined(ARDUINO_ARCH_ESP32) && defined(CONFIG_IDF_TARGET_ESP32P4)
#include <esp32-hal-ledc.h>
#endif

#if (defined(LITMUS_USE_JD9365) && LITMUS_USE_JD9365) || (defined(LITMUS_WAVESHARE_3C4) && LITMUS_WAVESHARE_3C4)
#include <Wire.h>
#endif
#if defined(LITMUS_USE_JD9365) && LITMUS_USE_JD9365
#include "waveshare_jd9365_720_init.h"
#endif
#if defined(LITMUS_WAVESHARE_3C4) && LITMUS_WAVESHARE_3C4
#include "waveshare_3inch4_dsi_800_init.h"
#endif

#if defined(LITMUS_WAVESHARE_3C4) && LITMUS_WAVESHARE_3C4
// Waveshare ESP32-P4-WIFI6-Touch-LCD-3.4C: 800×800 round, SH8601 — see their displays_config.h (SCREEN_3INCH_4_DSI).
#define LITMUS_LCD_W 800
#define LITMUS_LCD_H 800
#define LITMUS_DSI_HPW 20
#define LITMUS_DSI_HBP 20
#define LITMUS_DSI_HFP 40
#define LITMUS_DSI_VPW 4
#define LITMUS_DSI_VBP 12
#define LITMUS_DSI_VFP 24
#define LITMUS_DSI_DPI_HZ 80000000L
#define LITMUS_DSI_LANE_MBPS 1500
#elif defined(LITMUS_USE_JD9365) && LITMUS_USE_JD9365
#define LITMUS_LCD_W 720
#define LITMUS_LCD_H 720
#define LITMUS_DSI_HPW 20
#define LITMUS_DSI_HBP 20
#define LITMUS_DSI_HFP 40
#define LITMUS_DSI_VPW 4
#define LITMUS_DSI_VBP 12
#define LITMUS_DSI_VFP 24
#define LITMUS_DSI_DPI_HZ 80000000L
#define LITMUS_DSI_LANE_MBPS 1200
#elif defined(LITMUS_PANEL_1024X600) && LITMUS_PANEL_1024X600
// Stock Espressif ESP32-P4 Function EV kit: default LCD in esp-bsp is 7" 1024×600 (EK79007 class).
// GFX uses the same jd9165_init_operations with this timing (see JC1060P470 in Arduino_GFX_dev_device.h).
// If you flash the 720×720 profile against this panel, the screen stays blank.
#define LITMUS_LCD_W 1024
#define LITMUS_LCD_H 600
#define LITMUS_DSI_HPW 40
#define LITMUS_DSI_HBP 160
#define LITMUS_DSI_HFP 160
#define LITMUS_DSI_VPW 10
#define LITMUS_DSI_VBP 23
#define LITMUS_DSI_VFP 12
#define LITMUS_DSI_DPI_HZ 48000000L
#define LITMUS_DSI_LANE_MBPS 750
#else
#define LITMUS_LCD_W 720
#define LITMUS_LCD_H 720
#define LITMUS_DSI_HPW 8
#define LITMUS_DSI_HBP 64
#define LITMUS_DSI_HFP 64
#define LITMUS_DSI_VPW 4
#define LITMUS_DSI_VBP 16
#define LITMUS_DSI_VFP 16
#define LITMUS_DSI_DPI_HZ 56000000L
#define LITMUS_DSI_LANE_MBPS 750
#endif

// Espressif ESP32-P4 Function EV board: RST_LCD is GPIO27. Without a hardware reset pulse, JD9165
// init can succeed in software but the panel stays blank. Waveshare JD9365 kits: reset is often in the FPC chain.
#if defined(LITMUS_USE_JD9365) && LITMUS_USE_JD9365
#ifndef LITMUS_LCD_RST
#define LITMUS_LCD_RST GFX_NOT_DEFINED
#endif
#else
#ifndef LITMUS_LCD_RST
#define LITMUS_LCD_RST 27
#endif
#endif

#if (defined(LITMUS_USE_JD9365) && LITMUS_USE_JD9365) || (defined(LITMUS_WAVESHARE_3C4) && LITMUS_WAVESHARE_3C4)
#ifndef LITMUS_WAVESHARE_PMIC_I2C_ADDR
#define LITMUS_WAVESHARE_PMIC_I2C_ADDR 0x45
#endif
#ifndef LITMUS_WAVESHARE_PMIC_SDA
#define LITMUS_WAVESHARE_PMIC_SDA 7
#endif
#ifndef LITMUS_WAVESHARE_PMIC_SCL
#define LITMUS_WAVESHARE_PMIC_SCL 8
#endif
#endif

#ifndef LITMUS_BACKLIGHT_PIN
#define LITMUS_BACKLIGHT_PIN 26
#endif
#ifndef LITMUS_BACKLIGHT_USE_PWM
#define LITMUS_BACKLIGHT_USE_PWM 1
#endif
#ifndef LITMUS_BACKLIGHT_ACTIVE_LOW
#define LITMUS_BACKLIGHT_ACTIVE_LOW 0
#endif
#ifndef LITMUS_BACKLIGHT_LEVEL
#define LITMUS_BACKLIGHT_LEVEL 51
#endif

#if (defined(LITMUS_USE_JD9365) && LITMUS_USE_JD9365) || (defined(LITMUS_WAVESHARE_3C4) && LITMUS_WAVESHARE_3C4)
static void litmus_waveshare_p4_mipi_power_on() {
  Wire.begin(LITMUS_WAVESHARE_PMIC_SDA, LITMUS_WAVESHARE_PMIC_SCL);
  Wire.setClock(100000);
  auto wr = [](uint8_t reg, uint8_t val) {
    Wire.beginTransmission(LITMUS_WAVESHARE_PMIC_I2C_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission();
  };
  wr(0x95, 0x11);
  wr(0x95, 0x17);
  wr(0x96, 0x00);
  delay(100);
  wr(0x96, (uint8_t)LITMUS_BACKLIGHT_LEVEL);
  for (int i = 0; i < 10; i++) {
    delay(100);
    yield();
  }
  Serial.println("litmus: Waveshare PMIC I2C sequence done");
}
#endif

static void litmus_init_backlight() {
#if ((defined(LITMUS_USE_JD9365) && LITMUS_USE_JD9365) || (defined(LITMUS_WAVESHARE_3C4) && LITMUS_WAVESHARE_3C4)) && LITMUS_BACKLIGHT_PIN < 0
  Wire.beginTransmission(LITMUS_WAVESHARE_PMIC_I2C_ADDR);
  Wire.write(0x96);
  Wire.write((uint8_t)LITMUS_BACKLIGHT_LEVEL);
  if (Wire.endTransmission() != 0) {
    Serial.println("litmus: WARN I2C backlight failed");
  } else {
    Serial.println("litmus: backlight I2C 0x45/0x96");
  }
#elif LITMUS_BACKLIGHT_PIN >= 0
  const int pin = LITMUS_BACKLIGHT_PIN;
#if LITMUS_BACKLIGHT_USE_PWM && !LITMUS_BACKLIGHT_ACTIVE_LOW && defined(CONFIG_IDF_TARGET_ESP32P4)
  if (ledcAttach(pin, 20000, 8)) {
    ledcWrite(pin, LITMUS_BACKLIGHT_LEVEL);
    Serial.printf("litmus: backlight PWM GPIO %d\n", pin);
    return;
  }
#endif
  pinMode(pin, OUTPUT);
#if LITMUS_BACKLIGHT_ACTIVE_LOW
  digitalWrite(pin, LOW);
#else
  digitalWrite(pin, HIGH);
#endif
  Serial.printf("litmus: backlight GPIO %d\n", pin);
#else
  Serial.println("litmus: backlight disabled");
#endif
}

static const char *kLitmusDispTag = "litmus-disp";

Arduino_GFX *create_litmus_display() {
#if !defined(CONFIG_IDF_TARGET_ESP32P4)
  return nullptr;
#else
  ESP_LOGI(kLitmusDispTag, "create_litmus_display() start (%dx%d)", (int)LITMUS_LCD_W, (int)LITMUS_LCD_H);
#if (defined(LITMUS_USE_JD9365) && LITMUS_USE_JD9365) || (defined(LITMUS_WAVESHARE_3C4) && LITMUS_WAVESHARE_3C4)
  ESP_LOGI(kLitmusDispTag, "Waveshare path: PMIC I2C then panel init");
  litmus_waveshare_p4_mipi_power_on();
  ESP_LOGI(kLitmusDispTag, "PMIC done, alloc DSI panel");
#endif

  auto *bus = new Arduino_ESP32DSIPanel(
      LITMUS_DSI_HPW, LITMUS_DSI_HBP, LITMUS_DSI_HFP, LITMUS_DSI_VPW, LITMUS_DSI_VBP, LITMUS_DSI_VFP,
      LITMUS_DSI_DPI_HZ, LITMUS_DSI_LANE_MBPS);

#if defined(LITMUS_WAVESHARE_3C4) && LITMUS_WAVESHARE_3C4
  size_t ncmds = tsps_waveshare_3inch4_dsi_800_init_ops_len;
  auto *gfx = new Arduino_DSI_Display(LITMUS_LCD_W, LITMUS_LCD_H, bus, 0, true, LITMUS_LCD_RST,
                                      tsps_waveshare_3inch4_dsi_800_init_ops, ncmds, 0, 0, 0, 0);
#elif defined(LITMUS_USE_JD9365) && LITMUS_USE_JD9365
  size_t ncmds = tsps_waveshare_jd9365_720_init_ops_len;
  auto *gfx = new Arduino_DSI_Display(LITMUS_LCD_W, LITMUS_LCD_H, bus, 0, true, LITMUS_LCD_RST,
                                      tsps_waveshare_jd9365_720_init_ops, ncmds, 0, 0, 0, 0);
#else
  size_t ncmds = sizeof(jd9165_init_operations) / sizeof(lcd_init_cmd_t);
  auto *gfx = new Arduino_DSI_Display(LITMUS_LCD_W, LITMUS_LCD_H, bus, 0, true, LITMUS_LCD_RST,
                                       jd9165_init_operations, ncmds, 0, 0, 0, 0);
#endif

  ESP_LOGI(kLitmusDispTag, "calling gfx->begin() …");
  if (!gfx->begin()) {
    ESP_LOGE(kLitmusDispTag, "gfx->begin() failed");
    return nullptr;
  }

  ESP_LOGI(kLitmusDispTag, "gfx->begin() OK, backlight");
  litmus_init_backlight();
  ESP_LOGI(kLitmusDispTag, "create_litmus_display() done");
  return gfx;
#endif
}

#include <display_setup.h>

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_log.h>

#if defined(ARDUINO_ARCH_ESP32) && defined(CONFIG_IDF_TARGET_ESP32P4)
#include <esp32-hal-ledc.h>
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

// Most Waveshare / Espressif MIPI kits need the BL pin driven; without it the panel looks dead (black).
// If your backlight is hardwired on, set TSPS_BACKLIGHT_PIN=-1 in platformio.ini build_flags.
#ifndef TSPS_BACKLIGHT_PIN
#define TSPS_BACKLIGHT_PIN 26
#endif
#ifndef TSPS_BACKLIGHT_USE_PWM
#define TSPS_BACKLIGHT_USE_PWM 1
#endif
#ifndef TSPS_BACKLIGHT_ACTIVE_LOW
#define TSPS_BACKLIGHT_ACTIVE_LOW 0
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
  wr(0x96, 0xFF);
  for (int i = 0; i < 10; i++) {
    delay(100);
    yield();
  }
  Serial.println("TSPS: Waveshare display PMIC (I2C 0x45) power sequence done");
}
#endif

static const char *kTspsDispTag = "tsps-disp";

static void tsps_init_backlight() {
#if ((defined(TSPS_USE_JD9365) && TSPS_USE_JD9365) || (defined(TSPS_WAVESHARE_3C4) && TSPS_WAVESHARE_3C4)) && TSPS_BACKLIGHT_PIN < 0
  Wire.beginTransmission(TSPS_WAVESHARE_PMIC_I2C_ADDR);
  Wire.write(0x96);
  Wire.write(255);
  if (Wire.endTransmission() != 0) {
    Serial.println("TSPS: WARN I2C backlight (0x45/0x96) failed");
  } else {
    Serial.println("TSPS: backlight via Waveshare PMIC I2C");
  }
#elif TSPS_BACKLIGHT_PIN >= 0
  const int pin = TSPS_BACKLIGHT_PIN;
#if TSPS_BACKLIGHT_USE_PWM && !TSPS_BACKLIGHT_ACTIVE_LOW && defined(CONFIG_IDF_TARGET_ESP32P4)
  if (ledcAttach(pin, 20000, 8)) {
    ledcWrite(pin, 255);
    Serial.printf("TSPS: backlight PWM on GPIO %d\n", pin);
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
  ESP_LOGI(kTspsDispTag, "create_tsps_display() done");
  return gfx;
#endif
}

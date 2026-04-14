#include <Arduino.h>

#include <esp_log.h>

#if defined(LITMUS_SERIAL_ONLY) && LITMUS_SERIAL_ONLY

SET_LOOP_TASK_STACK_SIZE(8192);

static const char *TAG = "litmus-serial";

// No display / no GFX — proves loopTask + console without touching MIPI.
void setup() {
  esp_log_level_set("*", ESP_LOG_INFO);
  ESP_LOGI(TAG, "serial-only: setup start");
  Serial.begin(115200);
  delay(500);
  for (int i = 0; i < 25; i++) {
    ESP_LOGI(TAG, "tick %d", i);
    Serial.printf("[serial-only] tick %d\n", i);
    Serial.flush();
    delay(200);
  }
  ESP_LOGI(TAG, "serial-only: done — if you see this, CPU/USB/UART path is OK");
}

void loop() {
  static uint32_t n;
  ESP_LOGI(TAG, "alive %lu", (unsigned long)n);
  Serial.printf("[serial-only] alive %lu\n", (unsigned long)n++);
  Serial.flush();
  delay(3000);
}

#else

#include <Arduino_GFX_Library.h>
#include <boot_splash.h>

#include "display_init.h"

// Default loopTask stack is 8KB; MIPI DSI + panel init needs more headroom.
SET_LOOP_TASK_STACK_SIZE(24 * 1024);

static const char *TAG = "litmus";

namespace {

Arduino_GFX *g_gfx = nullptr;

}  // namespace

void setup() {
  esp_log_level_set("*", ESP_LOG_INFO);
  // Same channel as ROM boot / panic printf — often visible when USB-CDC `Serial` is not.
  ESP_LOGI(TAG, "setup entry");
  Serial.begin(115200);
  delay(400);
  ESP_LOGI(TAG, "after Serial.begin");
  Serial.println();
  Serial.println("esp32_p4_display_litmus — MIPI panel test");
  Serial.println("[litmus] next: create_litmus_display()");
  Serial.flush();

  g_gfx = create_litmus_display();
  if (!g_gfx) {
    ESP_LOGE(TAG, "create_litmus_display() returned nullptr");
    Serial.println("[litmus] FATAL: create_litmus_display() returned nullptr (begin() failed)");
    Serial.flush();
    while (true) {
      delay(500);
    }
  }

  ESP_LOGI(TAG, "display begin OK — drawing");
  Serial.printf("[litmus] display OK — drawing on %dx%d\n", g_gfx->width(), g_gfx->height());
  Serial.flush();

  elc_boot_splash_framebuffer_write(g_gfx);

  constexpr uint16_t kFg = 0xFFFF;  // white

  g_gfx->setTextSize(4, 4, 0);
  g_gfx->setTextColor(kFg);

  const char *msg = "HELLO";
  int16_t x1 = 0;
  int16_t y1 = 0;
  uint16_t w = 0;
  uint16_t h = 0;
  g_gfx->getTextBounds(msg, 0, 0, &x1, &y1, &w, &h);

  const int16_t x = (g_gfx->width() - (int16_t)w) / 2;
  const int16_t y = (g_gfx->height() - (int16_t)h) / 2;
  g_gfx->setCursor(x, y);
  g_gfx->print(msg);

  g_gfx->flush(true);

  ESP_LOGI(TAG, "drew \"%s\" at (%d,%d)", msg, x, y);
  Serial.printf("litmus: drew \"%s\" at (%d,%d) on %dx%d\n", msg, x, y, g_gfx->width(), g_gfx->height());
  Serial.flush();
}

void loop() {
  delay(5000);
}

#endif

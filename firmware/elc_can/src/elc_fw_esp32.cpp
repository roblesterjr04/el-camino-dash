// Only builds that opt in get the updater: firmware/scripts/elc_fw.py defines ELC_FW_VARIANT (the screens, the console).
// Other ESP32 boards that link this library (the audio card: one app slot, no room for an update) get the stubs below,
// and their image does not change.
#if defined(ARDUINO_ARCH_ESP32) && defined(ELC_FW_VARIANT)
#define ELC_FW_ENABLED 1
#else
#define ELC_FW_ENABLED 0
#endif

#include "elc_fw_esp32.h"

#if ELC_FW_ENABLED

#include <Arduino.h>
#include <Update.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <stdio.h>
#include <string.h>

#if ELC_FW_ROLLBACK
// Arduino's core accepts a new image as soon as it boots unless this says "later" (esp32-hal-misc.c). Later is when
// elc_fw_esp32_poll() has seen the board run healthy for ELC_FW_GOOD_MS. Only an image written over the air is ever
// "pending verify", so a board flashed over USB is not affected.
extern "C" bool verifyRollbackLater() { return true; }
#endif

namespace {

ElcFwTarget s_target;
ElcFwFlashEsp32 s_flash;
bool s_begun = false;
uint32_t s_healthy_since = 0;
bool s_good_done = false;
bool (*s_allowed)(void *) = nullptr;
void *s_allowed_ctx = nullptr;

bool allowed_thunk(void *) { return !s_allowed || s_allowed(s_allowed_ctx); }

const char *state_name(esp_ota_img_states_t s) {
  switch (s) {
    case ESP_OTA_IMG_NEW: return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "on trial";
    case ESP_OTA_IMG_VALID: return "good";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "rolled back";
    default: return "-";
  }
}

void commit_task(void *arg) {
  ((ElcFwFlashEsp32 *)arg)->run_commit();
  vTaskDelete(nullptr);
}

}  // namespace

// ---- the OTA slot ----

uint32_t ElcFwFlashEsp32::capacity() {
  const esp_partition_t *p = esp_ota_get_next_update_partition(nullptr);
  return p ? (uint32_t)p->size : 0;
}

bool ElcFwFlashEsp32::begin(uint32_t image_size) {
  abort();
  _err[0] = '\0';
  _size = image_size;
  _have = 0;
  if (image_size == 0 || image_size > capacity()) {
    snprintf(_err, sizeof(_err), "image %lu > slot %lu", (unsigned long)image_size, (unsigned long)capacity());
    return false;
  }
  // PSRAM staging, keeping 1 MB of PSRAM free for the rest of the board
  if (_allow_staging && heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >= image_size + (1u << 20)) {
    _stage = (uint8_t *)heap_caps_malloc(image_size, MALLOC_CAP_SPIRAM);
  }
  if (!_stage && !Update.begin(image_size, U_FLASH)) {
    snprintf(_err, sizeof(_err), "Update.begin: %s", Update.errorString());
    return false;
  }
  _open = true;
  return true;
}

bool ElcFwFlashEsp32::write(const uint8_t *p, size_t n) {
  if (!_open || _have + n > _size) return false;
  if (_stage) {
    memcpy(_stage + _have, p, n);
  } else if (Update.write((uint8_t *)p, n) != n) {
    snprintf(_err, sizeof(_err), "Update.write: %s", Update.errorString());
    return false;
  }
  _have += (uint32_t)n;
  return true;
}

void ElcFwFlashEsp32::run_commit() {
  uint8_t r = ELC_BULK_OK;
  if (!Update.begin(_size, U_FLASH)) {
    snprintf(_err, sizeof(_err), "Update.begin: %s", Update.errorString());
    r = ELC_BULK_NO_SPACE;
  } else {
    for (uint32_t off = 0; off < _size && r == ELC_BULK_OK;) {
      const size_t k = _size - off < 4096 ? _size - off : 4096;
      if (Update.write(_stage + off, k) != k) {
        snprintf(_err, sizeof(_err), "Update.write: %s", Update.errorString());
        r = ELC_BULK_NO_SPACE;
      }
      off += (uint32_t)k;
    }
    if (r == ELC_BULK_OK && !Update.end(false)) {
      snprintf(_err, sizeof(_err), "image: %s", Update.errorString());
      r = ELC_BULK_REJECTED;
    }
    if (r != ELC_BULK_OK) Update.abort();
  }
  heap_caps_free(_stage);
  _stage = nullptr;
  _commit_result = r;
  _committing = false;
}

uint8_t ElcFwFlashEsp32::commit() {
  if (!_open || _have != _size) return ELC_BULK_ABORTED;
  _open = false;
  if (_stage) {
    // flash is written from its own task so the loop (and CAN) keeps running: a few seconds per megabyte
    _committing = true;
    _commit_result = ELC_BULK_PENDING;
    if (xTaskCreate(commit_task, "elc_fw", 6144, this, 1, nullptr) != pdPASS) {
      _committing = false;
      run_commit();
      return _commit_result;
    }
    return ELC_BULK_PENDING;
  }
  if (!Update.end(false)) {
    snprintf(_err, sizeof(_err), "image: %s", Update.errorString());
    Update.abort();
    return ELC_BULK_REJECTED;
  }
  return ELC_BULK_OK;
}

uint8_t ElcFwFlashEsp32::committing() { return _committing ? ELC_BULK_PENDING : _commit_result; }

void ElcFwFlashEsp32::abort() {
  if (_committing) return;  // the task owns the slot until it is done
  if (_stage) {
    heap_caps_free(_stage);
    _stage = nullptr;
  } else if (_open) {
    Update.abort();
  }
  _open = false;
}

// ---- the board's target ----

void elc_fw_esp32_begin(ElcChannels *ch, uint16_t product, uint16_t fw_version) {
  if (s_begun || !ch) return;
  s_begun = true;
  s_target.begin(ch, &s_flash, product, ELC_FW_VARIANT, fw_version);
  s_target.setAllowed(allowed_thunk, nullptr);
  const esp_partition_t *run = esp_ota_get_running_partition();
  esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
  if (run) esp_ota_get_state_partition(run, &st);
  Serial.printf("fw: build %s v%u on %s (%s), update slot %lu KB\n", ELC_FW_VARIANT, (unsigned)fw_version,
                run ? run->label : "?", state_name(st), (unsigned long)(s_flash.capacity() / 1024));
  const esp_partition_t *other = esp_ota_get_next_update_partition(nullptr);
  esp_ota_img_states_t ost = ESP_OTA_IMG_UNDEFINED;
  if (other && esp_ota_get_state_partition(other, &ost) == ESP_OK &&
      (ost == ESP_OTA_IMG_ABORTED || ost == ESP_OTA_IMG_INVALID)) {
    Serial.printf("fw: the image in %s did not start well and was rolled back\n", other->label);
  }
}

void elc_fw_esp32_set_allowed(bool (*vehicle_stopped)(void *ctx), void *ctx) {
  s_allowed = vehicle_stopped;
  s_allowed_ctx = ctx;
}

ElcFwTarget *elc_fw_esp32_target() { return s_begun ? &s_target : nullptr; }

bool elc_fw_esp32_pending() {
  const esp_partition_t *run = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  return run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
}

void elc_fw_esp32_poll(uint32_t now_ms, bool healthy) {
  if (!s_begun) return;
  if (s_target.restartDue(now_ms)) {
    Serial.println("fw: update written, restarting into it");
    Serial.flush();
    delay(50);
    ESP.restart();
  }
  if (s_good_done) return;
  if (!healthy) {
    s_healthy_since = 0;
    return;
  }
  if (!s_healthy_since) s_healthy_since = now_ms ? now_ms : 1;
  if (now_ms - s_healthy_since >= ELC_FW_GOOD_MS) {
    s_good_done = true;
    if (elc_fw_esp32_pending()) {
      esp_ota_mark_app_valid_cancel_rollback();
      Serial.println("fw: the new image ran well; it is kept");
    }
  }
}

bool elc_fw_esp32_command(const char *line, char *reply, size_t n) {
  if (strncmp(line, "fw", 2) != 0 || (line[2] && line[2] != ' ')) return false;
  if (!strcmp(line, "fw good")) {
    if (elc_fw_esp32_pending()) {
      esp_ota_mark_app_valid_cancel_rollback();
      s_good_done = true;
      snprintf(reply, n, "fw: running image accepted");
    } else {
      snprintf(reply, n, "fw: nothing on trial");
    }
    return true;
  }
  char a[200];
  s_target.describe(a, sizeof(a));
  const esp_partition_t *run = esp_ota_get_running_partition();
  const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
  esp_ota_img_states_t rs = ESP_OTA_IMG_UNDEFINED, ns = ESP_OTA_IMG_UNDEFINED;
  if (run) esp_ota_get_state_partition(run, &rs);
  if (next) esp_ota_get_state_partition(next, &ns);
  snprintf(reply, n, "%s\nfw: running %s (%s), next %s (%s), %lu KB; PSRAM free block %lu KB", a, run ? run->label : "?",
           state_name(rs), next ? next->label : "?", state_name(ns), (unsigned long)(s_flash.capacity() / 1024),
           (unsigned long)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
  return true;
}

#else  // not an ESP32, or a build without elc_fw.py: nothing to update

#if defined(ARDUINO_ARCH_ESP32)
#include <Arduino.h>
void elc_fw_esp32_begin(ElcChannels *, uint16_t, uint16_t) {
  Serial.println("fw: this build takes no updates over CAN (ELC_FW_VARIANT not set: is scripts/elc_fw.py in extra_scripts?)");
}
#else
void elc_fw_esp32_begin(ElcChannels *, uint16_t, uint16_t) {}
#endif
void elc_fw_esp32_poll(uint32_t, bool) {}
void elc_fw_esp32_set_allowed(bool (*)(void *), void *) {}
ElcFwTarget *elc_fw_esp32_target() { return nullptr; }
bool elc_fw_esp32_pending() { return false; }
bool elc_fw_esp32_command(const char *, char *, size_t) { return false; }
uint32_t ElcFwFlashEsp32::capacity() { return 0; }
bool ElcFwFlashEsp32::begin(uint32_t) { return false; }
bool ElcFwFlashEsp32::write(const uint8_t *, size_t) { return false; }
uint8_t ElcFwFlashEsp32::commit() { return ELC_BULK_ABORTED; }
uint8_t ElcFwFlashEsp32::committing() { return ELC_BULK_ABORTED; }
void ElcFwFlashEsp32::abort() {}
void ElcFwFlashEsp32::run_commit() {}

#endif

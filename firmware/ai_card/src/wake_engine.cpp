// esp-sr WakeNet binding. NOT BUILT OR RUN IN THIS REPOSITORY'S CI: the API below follows esp-sr 1.x as bundled with
// Arduino-ESP32 3.x (esp_wn_iface.h / esp_wn_models.h / model_path.h); check the names against the version that
// platformio.ini pins when the card is first brought up, and fix here only.
#include "wake_engine.h"

#if defined(AI_WAKE_ENGINE) && AI_WAKE_ENGINE
#include <string.h>

#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"

bool WakeEngine::begin() {
  srmodel_list_t *models = esp_srmodel_init("model");
  if (!models) return false;
  char *name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
  if (!name) return false;
  const esp_wn_iface_t *wn = esp_wn_handle_from_name(name);
  if (!wn) return false;
  model_iface_data_t *d = wn->create(name, DET_MODE_90);
  if (!d) return false;
  _iface = wn;
  _data = d;
  _chunk = (size_t)wn->get_samp_chunksize(d);
  strncpy(_name, name, sizeof(_name) - 1);
  return _chunk > 0;
}

int WakeEngine::detect(const int16_t *pcm) {
  if (!_data) return 0;
  const esp_wn_iface_t *wn = static_cast<const esp_wn_iface_t *>(_iface);
  const wakenet_state_t st = wn->detect(static_cast<model_iface_data_t *>(_data), const_cast<int16_t *>(pcm));
  return st == WAKENET_DETECTED ? wn->get_triggered_channel(static_cast<model_iface_data_t *>(_data)) + 1 : 0;
}
#else
bool WakeEngine::begin() { return false; }
int WakeEngine::detect(const int16_t *) { return 0; }
#endif

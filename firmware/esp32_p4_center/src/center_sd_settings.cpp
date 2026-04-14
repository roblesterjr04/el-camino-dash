#include "center_sd_settings.h"

#if defined(BOARD_HAS_SDMMC) && defined(SOC_SDMMC_HOST_SUPPORTED)
#include <FS.h>
#include <SD_MMC.h>
#endif

#include "center_wifi_ntp.h"

static bool s_sd_mounted = false;

#if defined(BOARD_HAS_SDMMC) && defined(SOC_SDMMC_HOST_SUPPORTED)

// Waveshare ESP32-P4-WIFI6-Touch-LCD-3.4C TF slot (SDIO 4‑wire). Requires BOARD_SDMMC_SLOT=1 + setPins before begin.
#if defined(TSPS_WAVESHARE_3C4)
static constexpr int kSdClk = 43;
static constexpr int kSdCmd = 44;
static constexpr int kSdD0 = 39;
static constexpr int kSdD1 = 40;
static constexpr int kSdD2 = 41;
static constexpr int kSdD3 = 42;
#endif

static const char *kSdMount = "/sdcard";
static const char *kSettingsPath = "/sdcard/elcamino/settings.json";

static void append_json_escape(String *out, const String &val) {
  for (size_t i = 0; i < val.length(); i++) {
    const char c = val[(int)i];
    if (c == '"' || c == '\\') {
      *out += '\\';
    }
    *out += c;
  }
}

static String json_get_string(const String &j, const char *key) {
  const String k = String("\"") + key + "\":\"";
  int a = j.indexOf(k);
  if (a < 0) {
    return String();
  }
  a += (int)k.length();
  String out;
  while (a < (int)j.length()) {
    const char c = j.charAt((unsigned)a);
    if (c == '\\' && a + 1 < (int)j.length()) {
      out += j.charAt((unsigned)(a + 1));
      a += 2;
      continue;
    }
    if (c == '"') {
      break;
    }
    out += c;
    a++;
  }
  return out;
}

static bool json_has_key(const String &j, const char *key) {
  return j.indexOf(String("\"") + key + "\"") >= 0;
}

static bool json_get_view(const String &j, uint8_t *out) {
  const String k = "\"view\":";
  int a = j.indexOf(k);
  if (a < 0) {
    return false;
  }
  a += (int)k.length();
  while (a < (int)j.length() && (j.charAt((unsigned)a) == ' ' || j.charAt((unsigned)a) == '\t')) {
    a++;
  }
  long v = j.substring((unsigned)a).toInt();
  if (v < 0 || v > 255) {
    return false;
  }
  *out = (uint8_t)v;
  return true;
}

#endif

void center_sd_settings_begin() {
#if defined(BOARD_HAS_SDMMC) && defined(SOC_SDMMC_HOST_SUPPORTED)
#if defined(TSPS_WAVESHARE_3C4)
  if (!SD_MMC.setPins(kSdClk, kSdCmd, kSdD0, kSdD1, kSdD2, kSdD3)) {
    Serial.println("[sd] setPins failed");
    return;
  }
#endif
  if (!SD_MMC.begin(kSdMount, false /*4-bit*/)) {
    Serial.println("[sd] mount failed (no card or pins)");
    s_sd_mounted = false;
    return;
  }
  s_sd_mounted = true;
  Serial.printf("[sd] mounted at %s\n", kSdMount);
  if (!SD_MMC.exists("/sdcard/elcamino")) {
    if (!SD_MMC.mkdir("/sdcard/elcamino")) {
      Serial.println("[sd] mkdir elcamino failed");
    }
  }
#else
  Serial.println("[sd] SDMMC not supported on this build");
#endif
}

void center_sd_settings_import(Preferences &prefs) {
#if defined(BOARD_HAS_SDMMC) && defined(SOC_SDMMC_HOST_SUPPORTED)
  if (!s_sd_mounted) {
    return;
  }
  if (!SD_MMC.exists(kSettingsPath)) {
    return;
  }
  File f = SD_MMC.open(kSettingsPath, "r");
  if (!f || f.isDirectory()) {
    if (f) {
      f.close();
    }
    return;
  }
  String j = f.readString();
  f.close();
  j.trim();
  if (j.length() < 8 || j.charAt(0) != '{') {
    Serial.println("[sd] settings.json invalid");
    return;
  }

  // SD is a backup: only seed NVS when a key is missing (e.g. fresh flash / empty NVS). Never let a
  // stale settings.json overwrite Wi‑Fi or TZ the user already saved in NVS.
  if (json_has_key(j, "tz")) {
    const String tz = json_get_string(j, "tz");
    if (tz.length() > 0 && !prefs.isKey("tz")) {
      prefs.putString("tz", tz);
    }
  }
  if (json_has_key(j, "wifi_ssid")) {
    const String ssid = json_get_string(j, "wifi_ssid");
    if (ssid.length() > 0 && !prefs.isKey("wifi_ssid")) {
      prefs.putString("wifi_ssid", ssid);
      if (json_has_key(j, "wifi_psk")) {
        prefs.putString("wifi_psk", json_get_string(j, "wifi_psk"));
      }
    }
  }
  uint8_t v = 0;
  if (json_get_view(j, &v)) {
    prefs.putUChar("view", v);
  }
  Serial.println("[sd] imported elcamino/settings.json → NVS");
#else
  (void)prefs;
#endif
}

void center_sd_settings_sync(Preferences &prefs) {
#if defined(BOARD_HAS_SDMMC) && defined(SOC_SDMMC_HOST_SUPPORTED)
  if (!s_sd_mounted) {
    return;
  }
  String tz = prefs.getString("tz", CENTER_TZ_DEFAULT);
  String ssid = prefs.getString("wifi_ssid", "");
  String psk = prefs.getString("wifi_psk", "");
  const uint8_t view = prefs.getUChar("view", 0);

  String json;
  json.reserve(tz.length() + ssid.length() + psk.length() + 96);
  json = "{\"tz\":\"";
  append_json_escape(&json, tz);
  json += "\",\"wifi_ssid\":\"";
  append_json_escape(&json, ssid);
  json += "\",\"wifi_psk\":\"";
  append_json_escape(&json, psk);
  json += "\",\"view\":";
  json += (unsigned)view;
  json += '}';

  File f = SD_MMC.open(kSettingsPath, "w", true);
  if (!f) {
    Serial.println("[sd] open settings.json failed");
    return;
  }
  const size_t n = f.print(json);
  f.close();
  if (n != json.length()) {
    Serial.println("[sd] short write settings.json");
    return;
  }
  Serial.println("[sd] settings written to elcamino/settings.json");
#else
  (void)prefs;
#endif
}

#include "center_wifi_ntp.h"

#include "center_sd_settings.h"

#include <stdio.h>
#include <WiFi.h>
#include <WiFiType.h>
#include <string.h>
#include <time.h>

static Preferences *s_prefs = nullptr;
static bool s_sntp_started = false;
static wl_status_t s_last_wifi = WL_NO_SHIELD;

static void start_sntp_if_needed() {
  if (s_sntp_started) {
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  s_sntp_started = true;
}

void center_wifi_ntp_begin(Preferences &prefs) {
  s_prefs = &prefs;
  center_wifi_reload_tz(prefs);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  s_sntp_started = false;
  s_last_wifi = (wl_status_t)WiFi.status();
}

void center_wifi_reload_tz(Preferences &prefs) {
  String tz = prefs.getString("tz", CENTER_TZ_DEFAULT);
  setenv("TZ", tz.c_str(), 1);
  tzset();
}

void center_wifi_set_tz_posix(Preferences &prefs, const char *posix) {
  if (!posix || !posix[0]) {
    return;
  }
  prefs.putString("tz", posix);
  center_wifi_reload_tz(prefs);
  center_sd_settings_sync(prefs);
}

void center_wifi_set_tz_utc_offset_hours(Preferences &prefs, int utc_offset_hours) {
  char buf[20];
  if (utc_offset_hours == 0) {
    strncpy(buf, "GMT0", sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
  } else {
    snprintf(buf, sizeof(buf), "GMT%+d", -utc_offset_hours);
  }
  center_wifi_set_tz_posix(prefs, buf);
}

void center_wifi_try_connect_saved() {
  if (!s_prefs) {
    return;
  }
  String ssid = s_prefs->getString("wifi_ssid", "");
  if (ssid.length() == 0) {
    return;
  }
  String psk = s_prefs->getString("wifi_psk", "");
  // ESP32-P4 uses esp_wifi_remote / hosted stack; give the co-processor time after boot.
  delay(200);
  WiFi.begin(ssid.c_str(), psk.c_str());
}

void center_ntp_after_wifi_connected() {
  start_sntp_if_needed();
}

void center_wifi_ntp_loop() {
  wl_status_t st = (wl_status_t)WiFi.status();
  if (st == WL_CONNECTED && s_last_wifi != WL_CONNECTED) {
    s_sntp_started = false;
    start_sntp_if_needed();
  }
  if (st != WL_CONNECTED) {
    s_sntp_started = false;
  }
  s_last_wifi = st;
}

bool center_wall_epoch_ms(uint64_t *out_ms) {
  struct timeval tv {};
  if (gettimeofday(&tv, nullptr) != 0) {
    return false;
  }
  if (tv.tv_sec < 1700000000L) {
    return false;
  }
  *out_ms = (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)(tv.tv_usec / 1000);
  return true;
}

int center_wifi_scan(char out_ssid[][33], uint8_t *out_enc, int max_n) {
  if (max_n <= 0) {
    return 0;
  }
  int n = WiFi.scanNetworks(/*async=*/false, /*hidden=*/true);
  if (n <= 0) {
    return 0;
  }
  int m = 0;
  for (int i = 0; i < n && m < max_n; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0) {
      continue;
    }
    strncpy(out_ssid[m], s.c_str(), 32);
    out_ssid[m][32] = '\0';
    out_enc[m] = (uint8_t)WiFi.encryptionType(i);
    m++;
  }
  WiFi.scanDelete();
  return m;
}

bool center_wifi_join_and_save(Preferences &prefs, const char *ssid, const char *psk) {
  if (!ssid || !ssid[0]) {
    return false;
  }
  WiFi.disconnect(true, true);
  delay(80);
  if (psk && psk[0]) {
    WiFi.begin(ssid, psk);
  } else {
    WiFi.begin(ssid);
  }
  for (int i = 0; i < 200; i++) {
    if (WiFi.status() == WL_CONNECTED) {
      prefs.putString("wifi_ssid", ssid);
      if (psk && psk[0]) {
        prefs.putString("wifi_psk", psk);
      } else {
        prefs.putString("wifi_psk", "");
      }
      center_sd_settings_sync(prefs);
      s_sntp_started = false;
      center_ntp_after_wifi_connected();
      return true;
    }
    delay(50);
  }
  return false;
}

void center_wifi_forget(Preferences &prefs) {
  prefs.remove("wifi_ssid");
  prefs.remove("wifi_psk");
  WiFi.disconnect(true, true);
  s_sntp_started = false;
  center_sd_settings_sync(prefs);
}

static char s_psk_ssid[33];
static bool s_psk_wait = false;

void center_shell_set_wifi_psk_pending(const char *pending_ssid) {
  if (!pending_ssid || !pending_ssid[0]) {
    s_psk_wait = false;
    s_psk_ssid[0] = '\0';
    return;
  }
  strncpy(s_psk_ssid, pending_ssid, 32);
  s_psk_ssid[32] = '\0';
  s_psk_wait = true;
}

static void handle_shell_line(Preferences &prefs, const String &line) {
  if (line.length() == 0) {
    return;
  }
  if (line == "help") {
    Serial.println("Commands: wifi <ssid> [password] | forget | tz <POSIX_TZ> | psk <wpa_password>");
    Serial.println("Default tz if unset: " CENTER_TZ_DEFAULT " (DST: use full POSIX, e.g. tz EST5EDT,...)");
    return;
  }
  if (line == "forget") {
    center_wifi_forget(prefs);
    Serial.println("ok: wifi credentials cleared");
    return;
  }
  if (line.startsWith("tz ")) {
    String tz = line.substring(3);
    tz.trim();
    center_wifi_set_tz_posix(prefs, tz.c_str());
    Serial.println("ok: tz saved");
    return;
  }
  if (line.startsWith("wifi ")) {
    String rest = line.substring(5);
    rest.trim();
    int sp = rest.indexOf(' ');
    if (sp < 0) {
      if (!center_wifi_join_and_save(prefs, rest.c_str(), "")) {
        Serial.println("err: connect failed");
      } else {
        Serial.println("ok: connected (open?)");
      }
      return;
    }
    String ssid = rest.substring(0, sp);
    String pw = rest.substring(sp + 1);
    pw.trim();
    if (!center_wifi_join_and_save(prefs, ssid.c_str(), pw.c_str())) {
      Serial.println("err: connect failed");
    } else {
      Serial.println("ok: connected");
    }
    return;
  }
  if (line.startsWith("psk ") && s_psk_wait) {
    String pw = line.substring(4);
    pw.trim();
    if (pw.length() == 0) {
      Serial.println("err: empty psk");
      return;
    }
    if (center_wifi_join_and_save(prefs, s_psk_ssid, pw.c_str())) {
      Serial.println("ok: connected");
    } else {
      Serial.println("err: connect failed");
    }
    s_psk_wait = false;
    s_psk_ssid[0] = '\0';
    return;
  }
}

void center_shell_poll(Preferences &prefs) {
  static String acc;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      acc.trim();
      handle_shell_line(prefs, acc);
      acc.clear();
    } else if (acc.length() < 240) {
      acc += c;
    }
  }
}

static bool s_scan_async = false;

void center_wifi_scan_async_start() {
  WiFi.scanNetworks(/*async=*/true, /*hidden=*/true);
  s_scan_async = true;
}

int center_wifi_scan_async_poll(char out_ssid[][33], uint8_t *out_enc, int max_n, int *out_n) {
  if (!out_n || max_n <= 0) {
    return 2;
  }
  if (!s_scan_async) {
    return 2;
  }
  const int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) {
    return 0;
  }
  s_scan_async = false;
  if (n == WIFI_SCAN_FAILED || n < 0) {
    *out_n = 0;
    return 2;
  }
  int m = 0;
  for (int i = 0; i < n && m < max_n; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0) {
      continue;
    }
    strncpy(out_ssid[m], s.c_str(), 32);
    out_ssid[m][32] = '\0';
    out_enc[m] = (uint8_t)WiFi.encryptionType(i);
    m++;
  }
  WiFi.scanDelete();
  *out_n = m;
  return m > 0 ? 1 : 2;
}

static enum class JoinPhase : uint8_t { Idle, Running } s_join_phase = JoinPhase::Idle;
static uint32_t s_join_started_ms = 0;
static Preferences *s_join_prefs = nullptr;
static char s_join_ssid[33];
static char s_join_psk[65];

void center_wifi_join_async_start(Preferences &prefs, const char *ssid, const char *psk) {
  if (!ssid || !ssid[0]) {
    return;
  }
  strncpy(s_join_ssid, ssid, 32);
  s_join_ssid[32] = '\0';
  if (psk) {
    strncpy(s_join_psk, psk, 64);
    s_join_psk[64] = '\0';
  } else {
    s_join_psk[0] = '\0';
  }
  s_join_prefs = &prefs;
  WiFi.disconnect(true, true);
  delay(80);
  if (s_join_psk[0]) {
    WiFi.begin(s_join_ssid, s_join_psk);
  } else {
    WiFi.begin(s_join_ssid);
  }
  s_join_phase = JoinPhase::Running;
  s_join_started_ms = millis();
}

void center_wifi_join_saved_async_start(Preferences &prefs) {
  String ssid = prefs.getString("wifi_ssid", "");
  if (ssid.length() == 0) {
    return;
  }
  String psk = prefs.getString("wifi_psk", "");
  center_wifi_join_async_start(prefs, ssid.c_str(), psk.c_str());
}

int center_wifi_join_async_poll() {
  if (s_join_phase != JoinPhase::Running) {
    return 2;
  }
  if (WiFi.status() == WL_CONNECTED) {
    if (s_join_prefs) {
      s_join_prefs->putString("wifi_ssid", s_join_ssid);
      if (s_join_psk[0]) {
        s_join_prefs->putString("wifi_psk", s_join_psk);
      } else {
        s_join_prefs->putString("wifi_psk", "");
      }
      center_sd_settings_sync(*s_join_prefs);
    }
    s_sntp_started = false;
    center_ntp_after_wifi_connected();
    s_join_phase = JoinPhase::Idle;
    return 1;
  }
  if (millis() - s_join_started_ms > 18000) {
    s_join_phase = JoinPhase::Idle;
    return 2;
  }
  return 0;
}

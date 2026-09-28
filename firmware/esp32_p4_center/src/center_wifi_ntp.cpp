#include "center_wifi_ntp.h"

#include <esp_heap_caps.h>
#include <p4_screen.h>

#include "center_ui.h"

#include "center_sd_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <WiFi.h>
#include <WiFiType.h>
#include <string.h>
#include <time.h>

static Preferences *s_prefs = nullptr;
static wl_status_t s_last_wifi = WL_NO_SHIELD;

void center_wifi_ntp_begin(Preferences &prefs) {
  s_prefs = &prefs;
  center_wifi_reload_tz(prefs);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  s_last_wifi = (wl_status_t)WiFi.status();
}

void center_wifi_reload_tz(Preferences &prefs) {
  String t = prefs.getString("tz", CENTER_TZ_DEFAULT);
  if (t.length() == 0) {
    t = CENTER_TZ_DEFAULT;
  }
  setenv("TZ", t.c_str(), 1);
  tzset();
}

bool center_wifi_tz_build_posix_from_local_minus_utc(long local_minus_utc_sec, char *out, size_t out_cap) {
  if (!out || out_cap < 16) {
    return false;
  }
  /* Match Arduino cores/esp32/esp32-hal-time.c setTimeZone(-gmtOffset_sec, …): gmtOffset_sec is
   * local−UTC (same as timeapi `currentUtcOffset.seconds`). */
  const long off = -local_minus_utc_sec;
  if (off % 3600L != 0) {
    snprintf(out, out_cap, "UTC%ld:%02d:%02d", off / 3600L, abs((int)((off % 3600L) / 60L)),
             abs((int)(off % 60L)));
  } else {
    snprintf(out, out_cap, "UTC%ld", off / 3600L);
  }
  return true;
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
  char buf[40];
  const long sec = (long)utc_offset_hours * 3600L;
  if (!center_wifi_tz_build_posix_from_local_minus_utc(sec, buf, sizeof(buf))) {
    strncpy(buf, CENTER_TZ_DEFAULT, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
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
  /* SNTP disabled: system time is set from GPS RMC (UTC). */
}

static void shell_join_poll();

void center_wifi_ntp_loop() {
  shell_join_poll();
  wl_status_t st = (wl_status_t)WiFi.status();
  if (st == WL_CONNECTED && s_last_wifi != WL_CONNECTED) {
    if (s_prefs) {
      center_wifi_reload_tz(*s_prefs);
    }
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

void center_wifi_forget(Preferences &prefs) {
  prefs.remove("wifi_ssid");
  prefs.remove("wifi_psk");
  WiFi.disconnect(true, true);
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

/** Serial-shell joins use the async path too; center_wifi_ntp_loop() reports the result. */
static bool s_shell_join_pending = false;

static void shell_join_start(Preferences &prefs, const char *ssid, const char *psk) {
  center_wifi_join_async_start(prefs, ssid, psk);
  s_shell_join_pending = true;
  Serial.println("wifi: joining...");
}

static void handle_shell_line(Preferences &prefs, const String &line) {
  if (line.length() == 0) {
    return;
  }
  if (line == "shot" || line == "shot legacy") {
    // Screenshot of what the panel shows (or the legacy drawing layer); tools/screenshot.py drives this.
    p4_screen_shot(Serial, &Serial, 2000000, 115200, line == "shot legacy");
    return;
  }
  if (line.startsWith("tap ")) {  // bench: simulate a touch, "tap <x> <y> [hold_ms]"
    int x = 0, y = 0;
    unsigned long hold = 80;
    if (sscanf(line.c_str() + 4, "%d %d %lu", &x, &y, &hold) >= 2) {
      center_inject_touch((int16_t)x, (int16_t)y, (uint32_t)hold);
      Serial.printf("ok: tap %d,%d for %lu ms\n", x, y, hold);
    } else {
      Serial.println("err: tap <x> <y> [hold_ms]");
    }
    return;
  }
  if (line == "perf") {
    const P4ScreenStats st = p4_screen_stats();
    Serial.printf("ui: view=%d theme=%d overlay=%d fullscreen=%d\n", (int)center_ui_view_mode(), (int)center_ui_cluster_theme(),
                  (int)center_ui_overlay_open(), (int)center_ui_fullscreen());
    Serial.printf("perf: %.1f fps, drawing %.1f ms + LVGL %.1f ms per frame, %.1f of 50 bands changed/frame, %lu rows/s written back, %s (%lu flips/s), heap %lu KB internal / %lu KB PSRAM free\n",
                  (double)st.fps, (double)st.app_ms, (double)st.lvgl_ms, (double)st.bands_changed, (unsigned long)st.flushed_rows,
                  st.double_buffered ? "double-buffered" : "single-buffered", (unsigned long)st.flips,
                  (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                  (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    return;
  }
  if (line == "help") {
    Serial.println("Commands: shot [legacy] | perf | tap <x> <y> [ms] | wifi <ssid> [password] | forget | psk <wpa_password> | tz <posix>");
    Serial.println("Time: UTC from GPS RMC/ZDA; POSIX tz from coords when Wi‑Fi is up (timeapi.io), or set manually:");
    return;
  }
  if (line == "forget") {
    center_wifi_forget(prefs);
    Serial.println("ok: wifi credentials cleared");
    return;
  }
  if (line.startsWith("tz ")) {
    String rest = line.substring(3);
    rest.trim();
    if (rest.length() == 0) {
      Serial.println("err: tz <posix> (e.g. EST5EDT,M3.2.0/2,M11.1.0 or UTC4 for UTC−4 local)");
      return;
    }
    center_wifi_set_tz_posix(prefs, rest.c_str());
    Serial.println("ok: tz saved");
    return;
  }
  if (line.startsWith("wifi ")) {
    String rest = line.substring(5);
    rest.trim();
    int sp = rest.indexOf(' ');
    if (sp < 0) {
      shell_join_start(prefs, rest.c_str(), "");
      return;
    }
    String ssid = rest.substring(0, sp);
    String pw = rest.substring(sp + 1);
    pw.trim();
    shell_join_start(prefs, ssid.c_str(), pw.c_str());
    return;
  }
  if (line.startsWith("psk ") && s_psk_wait) {
    String pw = line.substring(4);
    pw.trim();
    if (pw.length() == 0) {
      Serial.println("err: empty psk");
      return;
    }
    shell_join_start(prefs, s_psk_ssid, pw.c_str());
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

static void shell_join_poll() {
  if (!s_shell_join_pending) {
    return;
  }
  const int r = center_wifi_join_async_poll();
  if (r == 0) {
    return;
  }
  s_shell_join_pending = false;
  Serial.println(r == 1 ? "ok: connected" : "err: connect failed");
}

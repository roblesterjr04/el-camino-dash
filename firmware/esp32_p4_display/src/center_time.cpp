#include "center_time.h"

#include "center_gps.h"
#include "center_sd_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <stdint.h>
#include <time.h>

static Preferences *s_prefs = nullptr;

void center_time_begin(Preferences &prefs) {
  s_prefs = &prefs;
  center_time_reload_tz(prefs);
}

void center_time_reload_tz(Preferences &prefs) {
  String t = prefs.getString("tz", CENTER_TZ_DEFAULT);
  if (t.length() == 0) {
    t = CENTER_TZ_DEFAULT;
  }
  setenv("TZ", t.c_str(), 1);
  tzset();
}

/** TZ string from seconds = local wall − UTC. Uses the `UTC±…` form of Arduino `configTime` / `setTimeZone`
 *  (POSIX sign: west of UTC is positive). */
static bool tz_build_posix_from_local_minus_utc(long local_minus_utc_sec, char *out, size_t out_cap) {
  if (!out || out_cap < 16) {
    return false;
  }
  const long off = -local_minus_utc_sec;
  if (off % 3600L != 0) {
    snprintf(out, out_cap, "UTC%ld:%02d:%02d", off / 3600L, abs((int)((off % 3600L) / 60L)),
             abs((int)(off % 60L)));
  } else {
    snprintf(out, out_cap, "UTC%ld", off / 3600L);
  }
  return true;
}

void center_time_set_tz_posix(Preferences &prefs, const char *posix) {
  if (!posix || !posix[0]) {
    return;
  }
  prefs.putString("tz", posix);
  center_time_reload_tz(prefs);
  center_sd_settings_sync(prefs);
}

void center_time_set_tz_utc_offset_hours(Preferences &prefs, int utc_offset_hours) {
  char buf[40];
  const long sec = (long)utc_offset_hours * 3600L;
  if (!tz_build_posix_from_local_minus_utc(sec, buf, sizeof(buf))) {
    strncpy(buf, CENTER_TZ_DEFAULT, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
  }
  center_time_set_tz_posix(prefs, buf);
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

void center_time_from_bus(uint32_t utc, int16_t offset_min) {
  if (center_gps_have_position_fix()) {
    return;  // GPS sets the clock
  }
  struct timeval now;
  gettimeofday(&now, nullptr);
  if (labs((long)now.tv_sec - (long)utc) > 1) {
    struct timeval tv = {(time_t)utc, 0};
    settimeofday(&tv, nullptr);
  }
  static int16_t s_applied = INT16_MIN;
  if (s_prefs && !s_prefs->isKey("tz") && offset_min != s_applied) {
    char posix[24];
    if (tz_build_posix_from_local_minus_utc((long)offset_min * 60L, posix, sizeof(posix))) {
      setenv("TZ", posix, 1);
      tzset();
      s_applied = offset_min;
    }
  }
}

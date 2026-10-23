#include "center_gps.h"

#include "center_time.h"
#include "center_ui.h"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#ifndef GPS_UART_RX
#define GPS_UART_RX 3  // DigiDash2 J6 pin 3 (from the device's TX) -> J1 pin 20 = GPIO3
#endif
#ifndef GPS_UART_TX
#define GPS_UART_TX 2  // J6 pin 2 (to the device's RX) -> J1 pin 22 = GPIO2
#endif
#ifndef GPS_UART_BAUD
#define GPS_UART_BAUD 9600
#endif
/** Set to 1 (e.g. build flag `-D CENTER_GPS_LOG_RAW_NMEA=1`) to log each NMEA line + hint on the sensor log. */
#ifndef CENTER_GPS_LOG_RAW_NMEA
#define CENTER_GPS_LOG_RAW_NMEA 0
#endif

/* Serial1 is the sensor link on the center board; GPS uses its own UART instance. */
static HardwareSerial s_gps(2);

static char s_line[192];
static size_t s_line_len;
static uint32_t s_last_uart_byte_ms = 0;

static double s_last_fix_lat = 0.0;
static double s_last_fix_lon = 0.0;
static bool s_have_fix_ll = false;

static time_t s_last_applied_sec = 0;

static Preferences *s_prefs = nullptr;

static char s_diag_ring[CENTER_GPS_DIAG_RECENT_LINES][128];

/** Interpret struct tm as UTC regardless of libc TZ (NMEA is always UTC). */
static time_t mktime_as_utc(struct tm *tm) {
  char saved_tz[48];
  const char *cur = getenv("TZ");
  if (cur) {
    strncpy(saved_tz, cur, sizeof(saved_tz) - 1);
    saved_tz[sizeof(saved_tz) - 1] = '\0';
  } else {
    saved_tz[0] = '\0';
  }
  setenv("TZ", "GMT0", 1);
  tzset();
  struct tm tmc = *tm;
  const time_t out = mktime(&tmc);
  if (saved_tz[0] != '\0') {
    setenv("TZ", saved_tz, 1);
    tzset();
  } else if (s_prefs) {
    center_time_reload_tz(*s_prefs);
  }
  return out;
}

static void diag_push_sentence(const char *sent) {
  if (!sent || sent[0] != '$') {
    return;
  }
  memmove(&s_diag_ring[0][0], &s_diag_ring[1][0],
          (size_t)(CENTER_GPS_DIAG_RECENT_LINES - 1) * sizeof(s_diag_ring[0]));
  strncpy(s_diag_ring[CENTER_GPS_DIAG_RECENT_LINES - 1], sent, 127);
  s_diag_ring[CENTER_GPS_DIAG_RECENT_LINES - 1][127] = '\0';
}

static int nmea_field(const char *line, int idx, char *out, size_t cap) {
  if (!out || cap == 0) {
    return -1;
  }
  out[0] = '\0';
  if (!line) {
    return -1;
  }
  int cur = 0;
  const char *p = line;
  if (*p == '$') {
    p++;
  }
  for (;;) {
    const char *comma = strchr(p, ',');
    if (cur == idx) {
      size_t n = comma ? (size_t)(comma - p) : strlen(p);
      /* Strip *checksum */
      const char *star = (const char *)memchr(p, '*', n);
      if (star) {
        n = (size_t)(star - p);
      }
      if (n >= cap) {
        n = cap - 1;
      }
      memcpy(out, p, n);
      out[n] = '\0';
      return (int)n;
    }
    if (!comma) {
      return -1;
    }
    p = comma + 1;
    cur++;
  }
}

static double nmea_coord_to_deg(const char *dm, char hemi) {
  if (!dm || !dm[0]) {
    return 0.0;
  }
  const char *dot = strchr(dm, '.');
  if (!dot || dot - dm < 3) {
    return 0.0;
  }
  int deg_digits = (int)(dot - dm) - 2;
  if (deg_digits < 1 || deg_digits > 3) {
    return 0.0;
  }
  char degs[5];
  if ((size_t)deg_digits >= sizeof(degs)) {
    return 0.0;
  }
  memcpy(degs, dm, (size_t)deg_digits);
  degs[deg_digits] = '\0';
  const int ideg = atoi(degs);
  const double mins = atof(dm + deg_digits);
  double v = (double)ideg + mins / 60.0;
  if (hemi == 'S' || hemi == 'W') {
    v = -v;
  }
  return v;
}

static bool zda_fields_to_epoch(const char *hhmmss, const char *day, const char *month, const char *year4, time_t *out) {
  if (!out || strlen(hhmmss) < 6 || strlen(day) < 1 || strlen(month) < 1 || strlen(year4) < 4) {
    return false;
  }
  int h = (hhmmss[0] - '0') * 10 + (hhmmss[1] - '0');
  int mi = (hhmmss[2] - '0') * 10 + (hhmmss[3] - '0');
  int s = (hhmmss[4] - '0') * 10 + (hhmmss[5] - '0');
  const int d = atoi(day);
  const int mo = atoi(month);
  const int y = atoi(year4);
  if (y < 2000 || y > 2100 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 60) {
    return false;
  }
  struct tm tm {};
  tm.tm_year = y - 1900;
  tm.tm_mon = mo - 1;
  tm.tm_mday = d;
  tm.tm_hour = h;
  tm.tm_min = mi;
  tm.tm_sec = s;
  tm.tm_isdst = -1;
  *out = mktime_as_utc(&tm);
  if (*out < (time_t)1700000000L) {
    return false;
  }
  return true;
}

static bool rmc_time_date_to_epoch(const char *hhmmss, const char *ddmmyy, time_t *out) {
  if (!out || strlen(ddmmyy) < 6 || strlen(hhmmss) < 6) {
    return false;
  }
  int h = (hhmmss[0] - '0') * 10 + (hhmmss[1] - '0');
  int mi = (hhmmss[2] - '0') * 10 + (hhmmss[3] - '0');
  int s = (hhmmss[4] - '0') * 10 + (hhmmss[5] - '0');
  int d = (ddmmyy[0] - '0') * 10 + (ddmmyy[1] - '0');
  int mo = (ddmmyy[2] - '0') * 10 + (ddmmyy[3] - '0');
  int y2 = (ddmmyy[4] - '0') * 10 + (ddmmyy[5] - '0');
  int y = 2000 + y2;
  /* Two-digit year: treat 00–79 as 2000–2079, 80–99 as 1980–1999 (matches common GPS / NMEA practice). */
  if (y2 >= 80) {
    y = 1900 + y2;
  }
  if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 60) {
    return false;
  }
  struct tm tm {};
  tm.tm_year = y - 1900;
  tm.tm_mon = mo - 1;
  tm.tm_mday = d;
  tm.tm_hour = h;
  tm.tm_min = mi;
  tm.tm_sec = s;
  tm.tm_isdst = -1;
  *out = mktime_as_utc(&tm);
  if (*out < (time_t)1700000000L) {
    return false;
  }
  return true;
}

static void apply_epoch(time_t epoch) {
  if (epoch != s_last_applied_sec) {
    struct timeval tv {};
    tv.tv_sec = epoch;
    tv.tv_usec = 0;
    (void)settimeofday(&tv, nullptr);
    s_last_applied_sec = epoch;
  }
}

/* RMC: apply UTC from time+date whenever fields parse (u‑blox often keeps UTC valid with status V).
 * Position only when status A and lat/lon fields look populated. */
static void handle_rmc_line(const char *line) {
  char tbuf[16], stat[4], latb[16], ns[2], lonb[16], ew[2], date[16];
  if (nmea_field(line, 1, tbuf, sizeof(tbuf)) < 6) {
    return;
  }
  if (nmea_field(line, 2, stat, sizeof(stat)) < 1) {
    return;
  }
  const bool pos_ok = (stat[0] == 'A');
  if (nmea_field(line, 9, date, sizeof(date)) >= 6) {
    time_t epoch = 0;
    if (rmc_time_date_to_epoch(tbuf, date, &epoch)) {
      apply_epoch(epoch);
    }
  }
  if (!pos_ok) {
    return;
  }
  if (nmea_field(line, 3, latb, sizeof(latb)) < 4) {
    return;
  }
  if (nmea_field(line, 4, ns, sizeof(ns)) < 1) {
    return;
  }
  if (nmea_field(line, 5, lonb, sizeof(lonb)) < 4) {
    return;
  }
  if (nmea_field(line, 6, ew, sizeof(ew)) < 1) {
    return;
  }
  const double lat = nmea_coord_to_deg(latb, ns[0]);
  const double lon = nmea_coord_to_deg(lonb, ew[0]);
  s_last_fix_lat = lat;
  s_last_fix_lon = lon;
  s_have_fix_ll = true;
}

/* $GPZDA / $GNZDA — full UTC calendar (often present when RMC is trimmed or delayed). */
static void handle_zda_line(const char *line) {
  char tbuf[16], dstr[8], mstr[8], ystr[8];
  if (nmea_field(line, 1, tbuf, sizeof(tbuf)) < 6) {
    return;
  }
  if (nmea_field(line, 2, dstr, sizeof(dstr)) < 1 || nmea_field(line, 3, mstr, sizeof(mstr)) < 1 ||
      nmea_field(line, 4, ystr, sizeof(ystr)) < 4) {
    return;
  }
  time_t epoch = 0;
  if (!zda_fields_to_epoch(tbuf, dstr, mstr, ystr, &epoch)) {
    return;
  }
  apply_epoch(epoch);
}

/* GGA: lat/lon + fix quality (no date), when GPS has a 2D/3D fix. */
static void handle_gga_line(const char *line) {
  char qual[8], latb[16], ns[2], lonb[16], ew[2];
  if (nmea_field(line, 6, qual, sizeof(qual)) < 1 || qual[0] < '1' || qual[0] > '9') {
    return; /* 0 = invalid; 1 = GPS SPS; 2 = DGPS; 4 = RTK fixed; etc. */
  }
  if (nmea_field(line, 2, latb, sizeof(latb)) < 4 || nmea_field(line, 3, ns, sizeof(ns)) < 1 ||
      nmea_field(line, 4, lonb, sizeof(lonb)) < 4 || nmea_field(line, 5, ew, sizeof(ew)) < 1) {
    return;
  }
  const double lat = nmea_coord_to_deg(latb, ns[0]);
  const double lon = nmea_coord_to_deg(lonb, ew[0]);
  if (lat == 0.0 && lon == 0.0) {
    return;
  }
  s_last_fix_lat = lat;
  s_last_fix_lon = lon;
  s_have_fix_ll = true;
}

static void try_parse_line(char *line) {
  if (strncmp(line, "$GPRMC", 6) == 0 || strncmp(line, "$GNRMC", 6) == 0 || strncmp(line, "$GLRMC", 6) == 0) {
    handle_rmc_line(line);
    return;
  }
  /* Seven chars: $ + talker (2) + sentence (4), e.g. $GPGGA */
  if (strncmp(line, "$GPGGA", 7) == 0 || strncmp(line, "$GNGGA", 7) == 0 || strncmp(line, "$GLGGA", 7) == 0) {
    handle_gga_line(line);
    return;
  }
  if (strncmp(line, "$GPZDA", 7) == 0 || strncmp(line, "$GNZDA", 7) == 0) {
    handle_zda_line(line);
    return;
  }
}

#if CENTER_GPS_LOG_RAW_NMEA
/* Sensor log line width (must match center_ui kSensorLogChars − 1 for strncpy). */
static constexpr int kGpsLogChars = 119;

/** Raw NMEA plus a short field map so comma-separated u‑blox output is recognizable. */
static void log_nmea_with_hint(const char *sentence) {
  center_ui_sensor_log_line(sentence);

  const size_t full_len = strlen(sentence);
  if ((int)full_len > kGpsLogChars) {
    char trunc[120];
    snprintf(trunc, sizeof(trunc), "  hint: line was %u chars (log shows first %d)", (unsigned)full_len, kGpsLogChars);
    center_ui_sensor_log_line(trunc);
  }

  char hint[120];
  if (strncmp(sentence, "$GPRMC", 6) == 0 || strncmp(sentence, "$GNRMC", 6) == 0 || strncmp(sentence, "$GLRMC", 6) == 0) {
    char t[16], st[4], la[16], ns[2], lo[16], ew[2], dt[16];
    nmea_field(sentence, 1, t, sizeof(t));
    nmea_field(sentence, 2, st, sizeof(st));
    nmea_field(sentence, 9, dt, sizeof(dt));
    const char fixc = (st[0] != '\0') ? st[0] : '?';
    const bool have_ll = nmea_field(sentence, 3, la, sizeof(la)) >= 4 && nmea_field(sentence, 4, ns, sizeof(ns)) >= 1 &&
                         nmea_field(sentence, 5, lo, sizeof(lo)) >= 4 && nmea_field(sentence, 6, ew, sizeof(ew)) >= 1;
    double latd = 0.0, lond = 0.0;
    if (have_ll && fixc == 'A') {
      latd = nmea_coord_to_deg(la, ns[0]);
      lond = nmea_coord_to_deg(lo, ew[0]);
      snprintf(hint, sizeof(hint),
               "  hint: RMC 1=hhmmss 2=A|V 3-4=lat 5-6=lon 9=ddmmyy | fix=%c t=%.6s d=%.6s lat=%.5f lon=%.5f", fixc, t,
               dt, latd, lond);
    } else {
      snprintf(hint, sizeof(hint), "  hint: RMC 1=UTC 2=A|V 3-4=lat 5-6=lon 9=ddmmyy | fix=%c t=%.6s d=%.6s", fixc, t,
               dt);
    }
    center_ui_sensor_log_line(hint);
    return;
  }
  if (strncmp(sentence, "$GPGGA", 7) == 0 || strncmp(sentence, "$GNGGA", 7) == 0 || strncmp(sentence, "$GLGGA", 7) == 0) {
    char tod[16], qual[8], la[16], ns[2], lo[16], ew[2];
    nmea_field(sentence, 1, tod, sizeof(tod));
    nmea_field(sentence, 6, qual, sizeof(qual));
    const char qc = (qual[0] != '\0') ? qual[0] : '?';
    if (nmea_field(sentence, 2, la, sizeof(la)) >= 4 && nmea_field(sentence, 3, ns, sizeof(ns)) >= 1 &&
        nmea_field(sentence, 4, lo, sizeof(lo)) >= 4 && nmea_field(sentence, 5, ew, sizeof(ew)) >= 1 && qc >= '1' &&
        qc <= '9') {
      const double latd = nmea_coord_to_deg(la, ns[0]);
      const double lond = nmea_coord_to_deg(lo, ew[0]);
      snprintf(hint, sizeof(hint),
               "  hint: GGA 1=UTC 2-3=lat 4-5=lon 6=fix(0void,1+=ok) | q=%c t=%.6s lat=%.5f lon=%.5f", qc, tod, latd,
               lond);
    } else {
      snprintf(hint, sizeof(hint), "  hint: GGA 1=UTC 2-3=lat 4-5=lon 6=fix(0=none,1=GPS,2=DGPS) q=%c t=%.6s", qc,
               tod);
    }
    center_ui_sensor_log_line(hint);
    return;
  }
  if (strncmp(sentence, "$GPZDA", 7) == 0 || strncmp(sentence, "$GNZDA", 7) == 0) {
    snprintf(hint, sizeof(hint), "  hint: ZDA 1=UTC 2=dd 3=mm 4=yyyy (full calendar, UTC)");
    center_ui_sensor_log_line(hint);
    return;
  }
  if ((strncmp(sentence, "$GPGSA", 7) == 0) || (strncmp(sentence, "$GNGSA", 7) == 0) || (strncmp(sentence, "$GLGSA", 7) == 0)) {
    center_ui_sensor_log_line("  hint: GSA = mode 2D/3D + PRNs used for navigation fix");
    return;
  }
  if ((strncmp(sentence, "$GPGSV", 7) == 0) || (strncmp(sentence, "$GNGSV", 7) == 0) || (strncmp(sentence, "$GLGSV", 7) == 0)) {
    center_ui_sensor_log_line("  hint: GSV = satellites in view (PRN, elev, azim, SNR per sat)");
    return;
  }
  if (strncmp(sentence, "$GPVTG", 7) == 0 || strncmp(sentence, "$GNVTG", 7) == 0) {
    center_ui_sensor_log_line("  hint: VTG = course and speed relative to ground");
    return;
  }
  snprintf(hint, sizeof(hint), "  hint: NMEA $TTSSS,… = talker+sentence; commas are standard field delimiters");
  center_ui_sensor_log_line(hint);
}
#endif /* CENTER_GPS_LOG_RAW_NMEA */

static void finish_line() {
  s_line[s_line_len] = '\0';
#if CENTER_GPS_LOG_RAW_NMEA
  if (s_line_len > 0 && s_line[0] == '$') {
    log_nmea_with_hint(s_line);
  }
#endif
  if (s_line_len > 0 && s_line[0] == '$') {
    diag_push_sentence(s_line);
  }
  if (s_line_len > 8) {
    try_parse_line(s_line);
  }
  s_line_len = 0;
}

void center_gps_begin(Preferences &prefs) {
  s_prefs = &prefs;
  s_line_len = 0;
  memset(s_diag_ring, 0, sizeof(s_diag_ring));
  s_have_fix_ll = false;
  s_gps.setRxBufferSize(4096);
  s_gps.begin(GPS_UART_BAUD, SERIAL_8N1, GPS_UART_RX, GPS_UART_TX);
}

void center_gps_loop() {
  while (s_gps.available()) {
    const int c = s_gps.read();
    if (c < 0) {
      break;
    }
    s_last_uart_byte_ms = millis();
    if (c == '\r') {
      /* NMEA often uses CR-only between sentences on UART bridges. */
      finish_line();
      continue;
    }
    if (c == '\n') {
      finish_line();
      continue;
    }
    if (s_line_len + 1 < sizeof(s_line)) {
      s_line[s_line_len++] = (char)c;
    } else {
      s_line_len = 0;
    }
  }
}

bool center_gps_have_position_fix() {
  return s_have_fix_ll;
}

uint32_t center_gps_last_uart_byte_ms() {
  return s_last_uart_byte_ms;
}

uint32_t center_gps_uart_baud() {
  return (uint32_t)GPS_UART_BAUD;
}

bool center_gps_last_fix_coords(double *out_lat, double *out_lon) {
  if (!out_lat || !out_lon) {
    return false;
  }
  *out_lat = s_last_fix_lat;
  *out_lon = s_last_fix_lon;
  return s_have_fix_ll;
}

void center_gps_diag_copy_partial(char *out, size_t cap) {
  if (!out || cap < 2) {
    return;
  }
  size_t n = s_line_len;
  if (n >= cap) {
    n = cap - 1u;
  }
  memcpy(out, s_line, n);
  out[n] = '\0';
}

void center_gps_diag_copy_recent(char out_rows[CENTER_GPS_DIAG_RECENT_LINES][128]) {
  if (!out_rows) {
    return;
  }
  memcpy(out_rows, s_diag_ring, sizeof(s_diag_ring));
}
